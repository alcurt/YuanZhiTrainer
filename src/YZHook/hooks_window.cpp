#include "hooks_window.h"

#include "hookmgr.h"
#include "yz_hook_state.h"

#include <map>
#include <string.h>

namespace
{
typedef BOOL (WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef BOOL (WINAPI *PFN_MoveWindow)(HWND, int, int, int, int, BOOL);
typedef BOOL (WINAPI *PFN_ShowWindow)(HWND, int);
#ifdef _WIN64
typedef LONG_PTR (WINAPI *PFN_SetWindowLongW)(HWND, int, LONG_PTR);
typedef LONG_PTR (WINAPI *PFN_SetWindowLongA)(HWND, int, LONG_PTR);
#else
typedef LONG (WINAPI *PFN_SetWindowLongW)(HWND, int, LONG);
typedef LONG (WINAPI *PFN_SetWindowLongA)(HWND, int, LONG);
#endif

PFN_SetWindowPos    g_realSetWindowPos    = nullptr;
PFN_MoveWindow      g_realMoveWindow      = nullptr;
PFN_ShowWindow      g_realShowWindow      = nullptr;
PFN_SetWindowLongW  g_realSetWindowLongW  = nullptr;
PFN_SetWindowLongA  g_realSetWindowLongA  = nullptr;

CRITICAL_SECTION     g_cs;
bool                 g_csInit = false;
std::map<HWND, DWORD> g_tracked;   /* hwnd -> 上次纠正 tick */
bool                 g_topmost = false;
bool                 g_hooksReady = false;
DWORD                g_windowizeCount = 0;

const wchar_t* const kSkipClasses[] =
{
    L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
    L"Button", L"Static", L"#32770"
};

void Lock()
{
    if (g_csInit)
        EnterCriticalSection(&g_cs);
}

void Unlock()
{
    if (g_csInit)
        LeaveCriticalSection(&g_cs);
}

bool IsSkippedClass(HWND hwnd)
{
    wchar_t cls[128] = {0};
    if (GetClassNameW(hwnd, cls, 128) == 0)
        return false;
    for (size_t i = 0; i < sizeof(kSkipClasses) / sizeof(kSkipClasses[0]); i++)
    {
        if (_wcsicmp(cls, kSkipClasses[i]) == 0)
            return true;
    }
    return false;
}

bool IsTracked(HWND hwnd)
{
    bool found = false;
    Lock();
    found = g_tracked.find(hwnd) != g_tracked.end();
    Unlock();
    return found;
}

void MarkTracked(HWND hwnd)
{
    Lock();
    g_tracked[hwnd] = GetTickCount();
    Unlock();
}

bool FullscreenCoversMonitor(const RECT& rc, const RECT& mon)
{
    const int tol = 3;
    return rc.left <= mon.left + tol && rc.top <= mon.top + tol &&
           rc.right >= mon.right - tol && rc.bottom >= mon.bottom - tol;
}

/* 判定“教师端全屏广播窗口”：本进程 + 顶层 + 无标题栏 + 覆盖整个显示器 + (POPUP 或置顶) */
bool IsCandidateWindow(HWND hwnd, RECT* outMonitor)
{
    if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
        return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr)
        return false;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId())
        return false;

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((style & WS_CHILD) != 0)
        return false;
    if ((style & WS_CAPTION) == WS_CAPTION)   /* 已有标题栏，说明已经是普通窗口 */
        return false;
    if (IsSkippedClass(hwnd))
        return false;

    bool popup   = (style & WS_POPUP) != 0;
    bool topmost = (ex & WS_EX_TOPMOST) != 0;
    if (!popup && !topmost)
        return false;

    RECT rc;
    if (!GetWindowRect(hwnd, &rc))
        return false;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return false;
    if (!FullscreenCoversMonitor(rc, mi.rcMonitor))
        return false;

    if (outMonitor != nullptr)
        *outMonitor = mi.rcMonitor;
    return true;
}

void ApplyWindowize(HWND hwnd, const RECT& mon)
{
    int percent = static_cast<int>(yzhook::g_windowPercent);
    if (percent < 20)
        percent = 20;
    if (percent > 100)
        percent = 100;

    const int monW = mon.right - mon.left;
    const int monH = mon.bottom - mon.top;
    if (monW <= 0 || monH <= 0)
        return;

    int w = monW * percent / 100;
    int h = static_cast<int>(static_cast<long long>(w) * monH / monW);
    if (h > monH)
        h = monH;
    int x = mon.left + (monW - w) / 2;
    int y = mon.top + (monH - h) / 2;

    yzhook::TryEnterHook();

    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);

    LONG newStyle = (style & ~(WS_POPUP | WS_MAXIMIZE | WS_DISABLED)) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    LONG newEx    = ex & ~(WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    if (g_topmost)
        newEx |= WS_EX_TOPMOST;

    if (newStyle != style)
        SetWindowLongW(hwnd, GWL_STYLE, newStyle);
    if (newEx != ex)
        SetWindowLongW(hwnd, GWL_EXSTYLE, newEx);

    SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, x, y, w, h,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    yzhook::LeaveHook();

    MarkTracked(hwnd);
    g_windowizeCount++;
    InterlockedIncrement(&yzhook::g_windowizeCount);

    yzhook::SendLogToHost(YZ_LOG_INFO,
        yz::Format(L"广播窗口已窗口化: 0x%p -> %d,%d %dx%d (置顶=%d)",
                       hwnd, x, y, w, h, g_topmost ? 1 : 0).c_str());
}

/* 该请求是否要把已窗口化的窗口重新变成全屏 */
bool IsRefullscreenRequest(HWND hwnd, int cx, int cy, UINT flags)
{
    if (flags & SWP_NOSIZE)
    {
        RECT rc;
        if (!GetWindowRect(hwnd, &rc))
            return false;
        cx = rc.right - rc.left;
        cy = rc.bottom - rc.top;
    }
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return false;
    const int monW = mi.rcMonitor.right - mi.rcMonitor.left;
    const int monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return (cx >= monW - 4 && cy >= monH - 4);
}

BOOL WINAPI Hook_SetWindowPos(HWND hwnd, HWND after, int x, int y, int cx, int cy, UINT flags)
{
    if (!yzhook::TryEnterHook())
        return g_realSetWindowPos(hwnd, after, x, y, cx, cy, flags);

    BOOL result = FALSE;
    RECT mon;
    if ((yzhook::g_flags & YZ_FLAG_WINDOWIZE) && IsCandidateWindow(hwnd, &mon))
    {
        ApplyWindowize(hwnd, mon);
        result = TRUE;
    }
    else if ((yzhook::g_flags & YZ_FLAG_WINDOWIZE) && IsTracked(hwnd) && IsRefullscreenRequest(hwnd, cx, cy, flags))
    {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            ApplyWindowize(hwnd, mi.rcMonitor);
        result = TRUE;
    }
    else
    {
        result = g_realSetWindowPos(hwnd, after, x, y, cx, cy, flags);
        if (result && (yzhook::g_flags & YZ_FLAG_WINDOWIZE) && IsCandidateWindow(hwnd, &mon))
            ApplyWindowize(hwnd, mon);
    }

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_MoveWindow(HWND hwnd, int x, int y, int cx, int cy, BOOL repaint)
{
    if (!yzhook::TryEnterHook())
        return g_realMoveWindow(hwnd, x, y, cx, cy, repaint);

    BOOL result = FALSE;
    RECT mon;
    if ((yzhook::g_flags & YZ_FLAG_WINDOWIZE) && IsTracked(hwnd) && IsRefullscreenRequest(hwnd, cx, cy, 0))
    {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            ApplyWindowize(hwnd, mi.rcMonitor);
        result = TRUE;
    }
    else
    {
        result = g_realMoveWindow(hwnd, x, y, cx, cy, repaint);
        if (result && (yzhook::g_flags & YZ_FLAG_WINDOWIZE) && IsCandidateWindow(hwnd, &mon))
            ApplyWindowize(hwnd, mon);
    }

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_ShowWindow(HWND hwnd, int cmdShow)
{
    if (!yzhook::TryEnterHook())
        return g_realShowWindow(hwnd, cmdShow);

    BOOL result = FALSE;
    if ((yzhook::g_flags & YZ_FLAG_WINDOWIZE) && IsTracked(hwnd) &&
        (cmdShow == SW_MAXIMIZE || cmdShow == SW_SHOWMAXIMIZED))
    {
        result = g_realShowWindow(hwnd, SW_SHOWNORMAL);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
            ApplyWindowize(hwnd, mi.rcMonitor);
    }
    else
    {
        result = g_realShowWindow(hwnd, cmdShow);
        if (result && (yzhook::g_flags & YZ_FLAG_WINDOWIZE))
        {
            RECT mon;
            if (IsCandidateWindow(hwnd, &mon))
                ApplyWindowize(hwnd, mon);
        }
    }

    yzhook::LeaveHook();
    return result;
}

LONG_PTR SanitizeLong(HWND hwnd, int index, LONG_PTR value)
{
    if (!IsTracked(hwnd))
        return value;

    if (index == GWL_STYLE)
    {
        LONG_PTR v = value;
        v &= ~(WS_POPUP | WS_MAXIMIZE | WS_DISABLED);
        v |= WS_OVERLAPPEDWINDOW;
        return v;
    }
    if (index == GWL_EXSTYLE)
    {
        LONG_PTR v = value;
        v &= ~WS_EX_TOOLWINDOW;
        if (g_topmost)
            v |= WS_EX_TOPMOST;
        else
            v &= ~WS_EX_TOPMOST;
        return v;
    }
    return value;
}

#ifdef _WIN64
LONG_PTR WINAPI Hook_SetWindowLongW(HWND hwnd, int index, LONG_PTR value)
#else
LONG WINAPI Hook_SetWindowLongW(HWND hwnd, int index, LONG value)
#endif
{
    if (!yzhook::TryEnterHook())
        return g_realSetWindowLongW(hwnd, index, value);

    LONG_PTR sanitized = value;
    if (yzhook::g_flags & YZ_FLAG_WINDOWIZE)
        sanitized = SanitizeLong(hwnd, index, value);

    LONG_PTR result = g_realSetWindowLongW(hwnd, index, sanitized);
    yzhook::LeaveHook();
    return result;
}

#ifdef _WIN64
LONG_PTR WINAPI Hook_SetWindowLongA(HWND hwnd, int index, LONG_PTR value)
#else
LONG WINAPI Hook_SetWindowLongA(HWND hwnd, int index, LONG value)
#endif
{
    if (!yzhook::TryEnterHook())
        return g_realSetWindowLongA(hwnd, index, value);

    LONG_PTR sanitized = value;
    if (yzhook::g_flags & YZ_FLAG_WINDOWIZE)
        sanitized = SanitizeLong(hwnd, index, value);

    LONG_PTR result = g_realSetWindowLongA(hwnd, index, sanitized);
    yzhook::LeaveHook();
    return result;
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM)
{
    if (!(yzhook::g_flags & YZ_FLAG_WINDOWIZE))
        return TRUE;

    RECT mon;
    if (IsCandidateWindow(hwnd, &mon))
    {
        DWORD now = GetTickCount();
        DWORD last = 0;
        Lock();
        std::map<HWND, DWORD>::iterator it = g_tracked.find(hwnd);
        if (it != g_tracked.end())
            last = it->second;
        Unlock();
        if (last == 0 || (now - last) >= 500)
            ApplyWindowize(hwnd, mon);
    }
    return TRUE;
}
} /* namespace */

namespace yzhook
{
bool WindowHooksInstall(bool enableNow)
{
    if (!g_csInit)
    {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
    if (g_hooksReady)
        return true;

    bool ok = true;
    ok = HookAttach("window", L"user32.dll", "SetWindowPos", reinterpret_cast<void*>(&Hook_SetWindowPos), enableNow) && ok;
    ok = HookAttach("window", L"user32.dll", "MoveWindow", reinterpret_cast<void*>(&Hook_MoveWindow), enableNow) && ok;
    ok = HookAttach("window", L"user32.dll", "ShowWindow", reinterpret_cast<void*>(&Hook_ShowWindow), enableNow) && ok;
    ok = HookAttach("window", L"user32.dll", "SetWindowLongW", reinterpret_cast<void*>(&Hook_SetWindowLongW), enableNow) && ok;
    ok = HookAttach("window", L"user32.dll", "SetWindowLongA", reinterpret_cast<void*>(&Hook_SetWindowLongA), enableNow) && ok;

    g_realSetWindowPos   = reinterpret_cast<PFN_SetWindowPos>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SetWindowPos)));
    g_realMoveWindow     = reinterpret_cast<PFN_MoveWindow>(HookGetOriginal(reinterpret_cast<void*>(&Hook_MoveWindow)));
    g_realShowWindow     = reinterpret_cast<PFN_ShowWindow>(HookGetOriginal(reinterpret_cast<void*>(&Hook_ShowWindow)));
    g_realSetWindowLongW = reinterpret_cast<PFN_SetWindowLongW>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SetWindowLongW)));
    g_realSetWindowLongA = reinterpret_cast<PFN_SetWindowLongA>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SetWindowLongA)));

    if (g_realSetWindowPos == nullptr || g_realShowWindow == nullptr)
    {
        YZLOGE(L"WindowHooksInstall: 关键原始函数指针为空，取消窗口 hook");
        return false;
    }

    g_hooksReady = true;
    YZLOGI(L"WindowHooksInstall: 完成 (enableNow=%d)", enableNow ? 1 : 0);
    return ok;
}

void WindowHooksShutdown()
{
    Lock();
    g_tracked.clear();
    Unlock();
    g_hooksReady = false;
}

void WindowSweepTick()
{
    if (!(g_flags & YZ_FLAG_WINDOWIZE))
        return;
    if (!g_hooksReady)
        return;
    EnumWindows(EnumProc, 0);
}

bool WindowIsTracked(HWND hwnd)
{
    return IsTracked(hwnd);
}

void WindowHooksSetTopmost(bool on)
{
    g_topmost = on;
    YZLOGI(L"窗口置顶开关: %d", on ? 1 : 0);
}

bool WindowHooksGetTopmost()
{
    return g_topmost;
}

void WindowHooksResetCount()
{
    g_windowizeCount = 0;
}

DWORD WindowHooksCount()
{
    return g_windowizeCount;
}
} /* namespace yzhook */

