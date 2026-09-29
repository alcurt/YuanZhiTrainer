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

/* 两种模式互斥：假全屏只去掉置顶、保留全屏外观，优先于"窗口化" */
bool FakeFullscreenMode() { return (yzhook::g_flags & YZ_FLAG_FAKE_FULLSCREEN) != 0; }
bool WindowizeMode()      { return (yzhook::g_flags & YZ_FLAG_WINDOWIZE) != 0; }
bool AnyWindowMode()      { return FakeFullscreenMode() || WindowizeMode(); }

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

/* 窗口化的目标几何：算一次、比一次、只在真的不一样时才写。
   09-29 实测：客户端会每秒二十次地重设广播窗口，如果每次都跟着 SetWindowPos，
   既闪屏（用户看到"全屏-窗口"横跳）又把日志刷爆。 */
struct WindowizeTarget
{
    LONG style;
    LONG ex;
    int  x;
    int  y;
    int  w;
    int  h;
};

bool ComputeWindowizeTarget(HWND hwnd, const RECT& mon, WindowizeTarget* out)
{
    int percent = static_cast<int>(yzhook::g_windowPercent);
    if (percent < 20)
        percent = 20;
    if (percent > 100)
        percent = 100;

    const int monW = mon.right - mon.left;
    const int monH = mon.bottom - mon.top;
    if (monW <= 0 || monH <= 0)
        return false;

    int w = monW * percent / 100;
    int h = static_cast<int>(static_cast<long long>(w) * monH / monW);
    if (h > monH)
        h = monH;

    out->x = mon.left + (monW - w) / 2;
    out->y = mon.top + (monH - h) / 2;
    out->w = w;
    out->h = h;

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);
    out->style = (style & ~(WS_POPUP | WS_MAXIMIZE | WS_DISABLED)) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    out->ex    = ex & ~(WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    if (g_topmost)
        out->ex |= WS_EX_TOPMOST;
    return true;
}

/* 已经在目标状态（带标题栏、不置顶、几何对得上）就什么都不用做 */
bool WindowizeSettled(HWND hwnd, const WindowizeTarget& t)
{
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((style & WS_POPUP) != 0 || (style & WS_DISABLED) != 0)
        return false;
    if ((style & WS_CAPTION) != WS_CAPTION)
        return false;
    if (((ex & WS_EX_TOPMOST) != 0) != g_topmost)
        return false;

    RECT rc;
    if (!GetWindowRect(hwnd, &rc))
        return false;
    const int tol = 4;
    return abs(rc.left - t.x) <= tol && abs(rc.top - t.y) <= tol &&
           abs((rc.right - rc.left) - t.w) <= tol && abs((rc.bottom - rc.top) - t.h) <= tol;
}

/* "广播窗口已窗口化"这类日志同样按 3 秒节流：客户端高频重设时它一秒能出二十条 */
void LogWindowEvent(HWND hwnd, const wchar_t* action, const WindowizeTarget& t)
{
    static DWORD s_lastTick = 0;
    static DWORD s_suppressed = 0;
    const DWORD  now = GetTickCount();
    if (s_lastTick != 0 && (now - s_lastTick) < 3000)
    {
        s_suppressed++;
        return;
    }

    yzhook::SendLogToHost(YZ_LOG_INFO,
        yz::Format(L"%s: 0x%p -> %d,%d %dx%d (置顶=%d)%s",
                   action, hwnd, t.x, t.y, t.w, t.h, g_topmost ? 1 : 0,
                   s_suppressed != 0 ? yz::Format(L"（此前 3 秒内抑制 %u 条同类日志）", s_suppressed).c_str() : L"")
            .c_str());
    s_lastTick  = now;
    s_suppressed = 0;
}

void ApplyWindowize(HWND hwnd, const RECT& mon)
{
    WindowizeTarget t = {0};
    if (!ComputeWindowizeTarget(hwnd, mon, &t))
        return;

    if (WindowizeSettled(hwnd, t))
    {
        /* 已达标：不写窗口、不记日志，**也不刷新追踪时间戳**——那个时间戳要留给
           "真正动过窗口"的时刻，否则 500ms 轮询那条兜底路径会被自己的刷新饿死。 */
        return;
    }

    /* 节流：同一窗口 200ms 内只真正改一次。客户端重设 → 我们拦下 → 客户端再重设，
       没有节流就是每秒二十次的互相拉扯。 */
    const DWORD now = GetTickCount();
    DWORD       last = 0;
    Lock();
    {
        std::map<HWND, DWORD>::iterator it = g_tracked.find(hwnd);
        if (it != g_tracked.end())
            last = it->second;
    }
    Unlock();
    if (last != 0 && (now - last) < 200)
        return;

    const bool entered = yzhook::TryEnterHook();

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (t.style != style)
        SetWindowLongW(hwnd, GWL_STYLE, t.style);
    if (t.ex != ex)
        SetWindowLongW(hwnd, GWL_EXSTYLE, t.ex);

    SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, t.x, t.y, t.w, t.h,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    if (entered)
        yzhook::LeaveHook();

    MarkTracked(hwnd);
    g_windowizeCount++;
    InterlockedIncrement(&yzhook::g_windowizeCount);
    LogWindowEvent(hwnd, L"广播窗口已窗口化", t);
}

/* 假全屏：保持"无边框铺满整块显示器"的外观，只把窗口从最上层拿下来。
   教师端看到的画面布局与正常全屏广播一样，但你 Alt+Tab 切过去的窗口能盖在它上面，
   配合键鼠解锁就能操作自己的电脑了。注意它不隐藏内容——教师端若在抓屏，
   看到的仍是你真实屏幕，要遮内容得靠防监视（冻结帧）。 */
void ApplyFakeFullscreen(HWND hwnd, const RECT& mon)
{
    const int monW = mon.right - mon.left;
    const int monH = mon.bottom - mon.top;
    if (monW <= 0 || monH <= 0)
        return;

    /* 同 ApplyWindowize：可能是嵌套调用，严格按 entered 成对释放 */
    const bool entered = yzhook::TryEnterHook();

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);

    /* 不加 WS_CAPTION：外观上仍是全屏广播；只清掉禁用态与置顶 */
    const LONG newStyle = (style & ~(WS_DISABLED | WS_MAXIMIZE)) | WS_VISIBLE;
    LONG newEx = ex & ~WS_EX_TOPMOST;
    if (g_topmost)
        newEx |= WS_EX_TOPMOST;

    if (newStyle != style)
        SetWindowLongW(hwnd, GWL_STYLE, newStyle);
    if (newEx != ex)
        SetWindowLongW(hwnd, GWL_EXSTYLE, newEx);

    SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 mon.left, mon.top, monW, monH,
                 SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    if (entered)
        yzhook::LeaveHook();

    MarkTracked(hwnd);
    g_windowizeCount++;
    InterlockedIncrement(&yzhook::g_windowizeCount);

    yzhook::SendLogToHost(YZ_LOG_INFO,
        yz::Format(L"广播窗口假全屏: 0x%p 保持 %dx%d 全屏、已取消置顶（置顶开关=%d）",
                   hwnd, monW, monH, g_topmost ? 1 : 0).c_str());
}

/* 已经处在"假全屏"稳态就不要再重复改样式（否则每秒都白改一次） */
bool FakeFullscreenDone(HWND hwnd)
{
    if (!IsTracked(hwnd))
        return false;
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((style & WS_CAPTION) == WS_CAPTION)
        return false;
    if ((style & WS_POPUP) == 0)
        return false;
    return (ex & WS_EX_TOPMOST) == 0;
}

/* 跟踪中的窗口又要求铺满整块显示器（远志每 3 秒抢一次全屏） */
void ReapplyForMode(HWND hwnd)
{
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        return;
    if (FakeFullscreenMode())
        ApplyFakeFullscreen(hwnd, mi.rcMonitor);
    else
        ApplyWindowize(hwnd, mi.rcMonitor);
}

/* 按当前模式处理一个候选窗口 */
void ApplyMode(HWND hwnd, const RECT& mon)
{
    if (FakeFullscreenMode())
        ApplyFakeFullscreen(hwnd, mon);
    else
        ApplyWindowize(hwnd, mon);
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
    if (AnyWindowMode() && IsCandidateWindow(hwnd, &mon))
    {
        if (FakeFullscreenMode() && FakeFullscreenDone(hwnd))
        {
            /* 已达假全屏稳态：几何照它说的改，但把"重新置顶"的意图压掉 */
            result = g_realSetWindowPos(hwnd, HWND_NOTOPMOST, x, y, cx, cy, flags & ~SWP_NOZORDER);
        }
        else
        {
            ApplyMode(hwnd, mon);
            result = TRUE;
        }
    }
    else if (AnyWindowMode() && IsTracked(hwnd) && IsRefullscreenRequest(hwnd, cx, cy, flags))
    {
        ReapplyForMode(hwnd);
        result = TRUE;
    }
    else
    {
        result = g_realSetWindowPos(hwnd, after, x, y, cx, cy, flags);
        if (result && AnyWindowMode() && IsCandidateWindow(hwnd, &mon) &&
            !(FakeFullscreenMode() && FakeFullscreenDone(hwnd)))
            ApplyMode(hwnd, mon);
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
    if (AnyWindowMode() && IsTracked(hwnd) && IsRefullscreenRequest(hwnd, cx, cy, 0))
    {
        ReapplyForMode(hwnd);
        result = TRUE;
    }
    else
    {
        result = g_realMoveWindow(hwnd, x, y, cx, cy, repaint);
        if (result && AnyWindowMode() && IsCandidateWindow(hwnd, &mon) &&
            !(FakeFullscreenMode() && FakeFullscreenDone(hwnd)))
            ApplyMode(hwnd, mon);
    }

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_ShowWindow(HWND hwnd, int cmdShow)
{
    if (!yzhook::TryEnterHook())
        return g_realShowWindow(hwnd, cmdShow);

    BOOL result = FALSE;
    if (AnyWindowMode() && IsTracked(hwnd) &&
        (cmdShow == SW_MAXIMIZE || cmdShow == SW_SHOWMAXIMIZED))
    {
        result = g_realShowWindow(hwnd, SW_SHOWNORMAL);
        ReapplyForMode(hwnd);
    }
    else
    {
        result = g_realShowWindow(hwnd, cmdShow);
        if (result && AnyWindowMode())
        {
            RECT mon;
            if (IsCandidateWindow(hwnd, &mon) && !(FakeFullscreenMode() && FakeFullscreenDone(hwnd)))
                ApplyMode(hwnd, mon);
        }
    }

    yzhook::LeaveHook();
    return result;
}

LONG_PTR SanitizeLong(HWND hwnd, int index, LONG_PTR value)
{
    if (!IsTracked(hwnd))
        return value;

    if (FakeFullscreenMode())
    {
        /* 假全屏：保留无边框全屏外观，只压掉置顶与禁用态，绝不加标题栏 */
        if (index == GWL_STYLE)
            return (value & ~(WS_DISABLED | WS_MAXIMIZE)) | WS_VISIBLE;
        if (index == GWL_EXSTYLE)
            return g_topmost ? (value | WS_EX_TOPMOST) : (value & ~WS_EX_TOPMOST);
        return value;
    }

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
    if (AnyWindowMode())
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
    if (AnyWindowMode())
        sanitized = SanitizeLong(hwnd, index, value);

    LONG_PTR result = g_realSetWindowLongA(hwnd, index, sanitized);
    yzhook::LeaveHook();
    return result;
}

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM)
{
    if (!AnyWindowMode())
        return TRUE;

    RECT mon;
    if (IsCandidateWindow(hwnd, &mon))
    {
        /* 假全屏的稳态窗口仍然是"无边框全屏"，别每秒重复改一遍 */
        if (FakeFullscreenMode() && FakeFullscreenDone(hwnd))
            return TRUE;

        DWORD now = GetTickCount();
        DWORD last = 0;
        Lock();
        std::map<HWND, DWORD>::iterator it = g_tracked.find(hwnd);
        if (it != g_tracked.end())
            last = it->second;
        Unlock();
        if (last == 0 || (now - last) >= 500)
            ApplyMode(hwnd, mon);
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

    /* 两阶段安装，理由同 CaptureHooksInstall：绝不能在 g_real* 为空时让钩子生效。 */
    ok = HookAttach("window", L"user32.dll", "SetWindowPos", reinterpret_cast<void*>(&Hook_SetWindowPos), false) && ok;
    ok = HookAttach("window", L"user32.dll", "MoveWindow", reinterpret_cast<void*>(&Hook_MoveWindow), false) && ok;
    ok = HookAttach("window", L"user32.dll", "ShowWindow", reinterpret_cast<void*>(&Hook_ShowWindow), false) && ok;
    ok = HookAttach("window", L"user32.dll", "SetWindowLongW", reinterpret_cast<void*>(&Hook_SetWindowLongW), false) && ok;
    ok = HookAttach("window", L"user32.dll", "SetWindowLongA", reinterpret_cast<void*>(&Hook_SetWindowLongA), false) && ok;

    g_realSetWindowPos   = reinterpret_cast<PFN_SetWindowPos>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SetWindowPos)));
    g_realMoveWindow     = reinterpret_cast<PFN_MoveWindow>(HookGetOriginal(reinterpret_cast<void*>(&Hook_MoveWindow)));
    g_realShowWindow     = reinterpret_cast<PFN_ShowWindow>(HookGetOriginal(reinterpret_cast<void*>(&Hook_ShowWindow)));
    g_realSetWindowLongW = reinterpret_cast<PFN_SetWindowLongW>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SetWindowLongW)));
    g_realSetWindowLongA = reinterpret_cast<PFN_SetWindowLongA>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SetWindowLongA)));

    if (g_realSetWindowPos == nullptr || g_realMoveWindow == nullptr || g_realShowWindow == nullptr ||
        g_realSetWindowLongW == nullptr || g_realSetWindowLongA == nullptr)
    {
        YZLOGE(L"WindowHooksInstall: 原始函数指针不完整，窗口 hook 保持停用");
        return false;
    }

    if (enableNow && !HookSetGroupEnabled("window", true))
    {
        YZLOGE(L"WindowHooksInstall: 启用窗口 hook 失败");
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
    if (!AnyWindowMode())
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

