#include "winfix.h"

#include "app.h"

#include "yz_log.h"
#include "yz_util.h"

#include <map>
#include <string>
#include <vector>

namespace
{
/* 扫描节流：看门狗本身 1 秒一跳，这里再兜一层，避免"立即注入"按钮触发重复扫描 */
const DWORD kScanIntervalMs = 900;
/* 同一个窗口的重新纠正间隔：远志抢回全屏是周期性的，太快纠正会来回打架 */
const DWORD kRecorrectMs    = 1000;

CRITICAL_SECTION      g_cs;
bool                  g_csInit = false;
std::map<HWND, DWORD> g_lastFix;

volatile LONG g_skipPid     = 0;
volatile LONG g_corrections = 0;
volatile LONG g_candidates  = 0;
volatile LONG g_writeFailed = 0;
volatile LONG g_warnLogged  = 0;

bool  g_topmost   = false;
DWORD g_lastScan  = 0;

const wchar_t* const kSkipClasses[] =
{
    L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
    L"Button", L"Static", L"#32770"
};

void Lock()   { if (g_csInit) EnterCriticalSection(&g_cs); }
void Unlock() { if (g_csInit) LeaveCriticalSection(&g_cs); }

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

bool FullscreenCoversMonitor(const RECT& rc, const RECT& mon)
{
    const int tol = 3;
    return rc.left <= mon.left + tol && rc.top <= mon.top + tol &&
           rc.right >= mon.right - tol && rc.bottom >= mon.bottom - tol;
}

bool NameInList(const std::wstring& name)
{
    if (name.empty())
        return false;
    for (size_t i = 0; i < g_app.cfg.processNames.size(); i++)
    {
        if (_wcsicmp(g_app.cfg.processNames[i].c_str(), name.c_str()) == 0)
            return true;
    }
    return false;
}

/* 这个窗口的进程是否属于远志。判据与注入器的目标打分保持一致：
   安装路径前缀 / ini 里的进程名名单。注意这里是在主程序里看别人的进程，
   所以必须做归属判断——否则一个无关的全屏播放器也会被我们窗口化。 */
bool ProcessIsYuanzhi(DWORD pid)
{
    const std::wstring path = yz::GetProcessImagePath(pid);
    if (!path.empty())
    {
        if (!g_app.cfg.targetDir.empty() && yz::IsUnderDir(path, g_app.cfg.targetDir))
            return true;
        if (yz::IsYuanzhiInstallPath(path))
            return true;
    }
    if (NameInList(yz::FileNameOf(path)))
        return true;
    /* 路径读不到（权限不足）时的兜底：注入器已经按"进程名/安装路径/远志模块"
       把目标判过一次，那个 PID 可以信；其余一律不认，避免误伤无关全屏窗口。 */
    return pid != 0 && g_app.targetPid != 0 && pid == g_app.targetPid;
}

/* 与 YZHook 侧 IsCandidateWindow 用同一套判据，只是这里看的是别人的窗口：
   顶层 + 可见 + 无属主 + 无标题栏 + 非子窗口 + 非桌面壳类 + 覆盖整块显示器 + (POPUP 或置顶)。 */
bool IsCandidateWindow(HWND hwnd, RECT* outMonitor)
{
    if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
        return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr)
        return false;
    if (IsSkippedClass(hwnd))
        return false;

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((style & WS_CHILD) != 0)
        return false;
    if ((style & WS_CAPTION) == WS_CAPTION)   /* 已有标题栏，说明已经是被窗口化过的普通窗口 */
        return false;
    if ((style & WS_POPUP) == 0 && (ex & WS_EX_TOPMOST) == 0)
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

/* 写失败要能看见：对 SYSTEM 完整性进程的窗口做跨进程样式写入是本模块最大的不确定点，
   一旦被拒（UIPI/win32k 权限检查），日志里必须留证据，而不是静默什么都没发生。 */
void LogWriteFailure(const wchar_t* stage, HWND hwnd, DWORD err)
{
    const LONG n = InterlockedIncrement(&g_warnLogged);
    if (n <= 3 || (n % 30) == 0)
    {
        const std::wstring text = yz::Format(L"外部窗口纠正%s失败: hwnd=0x%p err=%s（累计 %d 次）",
                                             stage, hwnd, yz::Win32ErrorMessage(err).c_str(), n);
        YZLOGW(L"%s", text.c_str());        /* 落盘：机房上要能靠日志判读 */
        UiAppendLog(yz::kLogWarn, text);
    }
    InterlockedExchange(&g_writeFailed, 1);
}

/* 跨进程改样式：返回值可能合法地为 0，所以必须自己清空并检查 LastError */
bool SetWindowLongChecked(HWND hwnd, int index, LONG_PTR value, const wchar_t* stage)
{
    SetLastError(0);
    LONG_PTR prev = SetWindowLongPtrW(hwnd, index, value);
    if (prev == 0)
    {
        const DWORD err = GetLastError();
        if (err != 0)
        {
            LogWriteFailure(stage, hwnd, err);
            return false;
        }
    }
    return true;
}

void ApplyWindowize(HWND hwnd, const RECT& mon)
{
    int percent = static_cast<int>(g_app.cfg.windowPercent);
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
    const int x = mon.left + (monW - w) / 2;
    const int y = mon.top + (monH - h) / 2;

    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR ex    = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    const LONG_PTR newStyle = (style & ~(WS_POPUP | WS_MAXIMIZE | WS_DISABLED)) |
                              WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    LONG_PTR newEx = ex & ~(WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    if (g_topmost)
        newEx |= WS_EX_TOPMOST;

    bool ok = true;
    if (newStyle != style)
        ok = SetWindowLongChecked(hwnd, GWL_STYLE, newStyle, L"写样式") && ok;
    if (newEx != ex)
        ok = SetWindowLongChecked(hwnd, GWL_EXSTYLE, newEx, L"写扩展样式") && ok;

    /* SWP_ASYNCWINDOWPOS：目标线程可能正卡在远志自己的模态循环里，
       同步 SetWindowPos 会连带阻塞我们的看门狗线程。 */
    SetLastError(0);
    if (!SetWindowPos(hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, x, y, w, h,
                      SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_ASYNCWINDOWPOS))
    {
        const DWORD err = GetLastError();
        if (err != 0)
        {
            LogWriteFailure(L"定位", hwnd, err);
            ok = false;
        }
    }

    if (!ok)
        return;

    const LONG total = InterlockedIncrement(&g_corrections);
    YZLOGI(L"外部窗口纠正: 0x%p -> %d,%d %dx%d（第 %d 次）", hwnd, x, y, w, h, total);
    /* 界面只报前几次与每 20 次，避免日志框被刷屏（文件日志始终全量） */
    if (total <= 5 || (total % 20) == 0)
        UiAppendLog(yz::kLogInfo, yz::Format(L"外部窗口纠正: 0x%p -> %d,%d %dx%d（第 %d 次）",
                                             hwnd, x, y, w, h, total));
}

struct ScanContext
{
    DWORD now;
    DWORD candidates;
};

BOOL CALLBACK EnumProc(HWND hwnd, LPARAM lParam)
{
    ScanContext* ctx = reinterpret_cast<ScanContext*>(lParam);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId())
        return TRUE;

    /* 进程内 Hook 正在管这个进程，别去抢同一个窗口 */
    const DWORD skipPid = static_cast<DWORD>(InterlockedCompareExchange(&g_skipPid, 0, 0));
    if (skipPid != 0 && pid == skipPid)
        return TRUE;

    RECT mon;
    if (!IsCandidateWindow(hwnd, &mon))
        return TRUE;
    if (!ProcessIsYuanzhi(pid))
        return TRUE;

    ctx->candidates++;

    Lock();
    std::map<HWND, DWORD>::iterator it = g_lastFix.find(hwnd);
    const DWORD last = (it != g_lastFix.end()) ? it->second : 0;
    Unlock();

    if (last != 0 && (ctx->now - last) < kRecorrectMs)
        return TRUE;

    ApplyWindowize(hwnd, mon);

    Lock();
    g_lastFix[hwnd] = ctx->now;
    Unlock();
    return TRUE;
}
} /* namespace */

namespace winfix
{
void WinFixTick()
{
    if (!g_csInit)
    {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }

    if (g_app.examMode)
        return;
    if ((g_app.cfg.flags & YZ_FLAG_WINDOWIZE) == 0)
        return;

    EnterCriticalSection(&g_cs);
    const DWORD now = GetTickCount();
    const bool tooSoon = (g_lastScan != 0) && ((now - g_lastScan) < kScanIntervalMs);
    if (!tooSoon)
        g_lastScan = now;
    LeaveCriticalSection(&g_cs);
    if (tooSoon)
        return;

    ScanContext ctx;
    ctx.now        = now;
    ctx.candidates = 0;
    EnumWindows(EnumProc, reinterpret_cast<LPARAM>(&ctx));

    InterlockedExchange(&g_candidates, static_cast<LONG>(ctx.candidates));

    /* 窗口销毁后句柄可能被复用，定期清掉太久没见到的记录 */
    Lock();
    for (std::map<HWND, DWORD>::iterator it = g_lastFix.begin(); it != g_lastFix.end(); )
    {
        if (!IsWindow(it->first) || (now - it->second) > 60000)
            it = g_lastFix.erase(it);
        else
            ++it;
    }
    Unlock();
}

void WinFixSetSkipPid(DWORD pid)
{
    InterlockedExchange(&g_skipPid, static_cast<LONG>(pid));
}

void WinFixSetTopmost(bool on)
{
    g_topmost = on;
}

DWORD WinFixCount()
{
    return static_cast<DWORD>(InterlockedCompareExchange(&g_corrections, 0, 0));
}

void WinFixStats(DWORD* corrections, DWORD* candidates, bool* writeFailed)
{
    if (corrections != nullptr)
        *corrections = static_cast<DWORD>(InterlockedCompareExchange(&g_corrections, 0, 0));
    if (candidates != nullptr)
        *candidates = static_cast<DWORD>(InterlockedCompareExchange(&g_candidates, 0, 0));
    if (writeFailed != nullptr)
        *writeFailed = InterlockedCompareExchange(&g_writeFailed, 0, 0) != 0;
}
} /* namespace winfix */
