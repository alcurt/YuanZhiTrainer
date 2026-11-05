#include "hooks_capture.h"

#include "hookmgr.h"
#include "yz_hook_state.h"

#include <set>
#include <string.h>

namespace
{
typedef HDC  (WINAPI *PFN_GetDC)(HWND);
typedef HDC  (WINAPI *PFN_GetWindowDC)(HWND);
typedef int  (WINAPI *PFN_ReleaseDC)(HWND, HDC);
typedef HDC  (WINAPI *PFN_CreateDCW)(LPCWSTR, LPCWSTR, LPCWSTR, const DEVMODEW*);
typedef BOOL (WINAPI *PFN_DeleteDC)(HDC);
typedef BOOL (WINAPI *PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef BOOL (WINAPI *PFN_StretchBlt)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);
typedef BOOL (WINAPI *PFN_PrintWindow)(HWND, HDC, UINT);
typedef HDC  (WINAPI *PFN_GetDCEx)(HWND, HRGN, DWORD);
typedef int  (WINAPI *PFN_GetDIBits)(HDC, HBITMAP, UINT, UINT, LPVOID, LPBITMAPINFO, UINT);

PFN_GetDC       g_realGetDC       = nullptr;
PFN_GetWindowDC g_realGetWindowDC = nullptr;
PFN_ReleaseDC   g_realReleaseDC   = nullptr;
PFN_CreateDCW   g_realCreateDCW   = nullptr;
PFN_DeleteDC    g_realDeleteDC    = nullptr;
PFN_BitBlt      g_realBitBlt      = nullptr;
PFN_StretchBlt  g_realStretchBlt  = nullptr;
PFN_PrintWindow g_realPrintWindow = nullptr;
PFN_GetDCEx     g_realGetDCEx     = nullptr;   /* v0.6.59：老代码常用 GetDCEx 取 DC */
PFN_GetDIBits   g_realGetDIBits   = nullptr;   /* v0.6.59：观测用，不改变行为 */

CRITICAL_SECTION g_cs;
bool             g_csInit = false;
std::set<HDC>    g_screenDCs;
std::set<HDC>    g_windowDCs;   /* v0.6.55：任意窗口 DC（不要求全屏） */
HDC     g_frozenDC   = nullptr;
HBITMAP g_frozenBmp  = nullptr;
void*   g_frozenBits = nullptr;
int     g_vx = 0;
int     g_vy = 0;
int     g_vw = 0;
int     g_vh = 0;
bool    g_frozen    = false;
bool    g_hooksReady = false;
volatile LONG g_redirectCount = 0;

void Lock()   { if (g_csInit) EnterCriticalSection(&g_cs); }
void Unlock() { if (g_csInit) LeaveCriticalSection(&g_cs); }

/* v0.6.55：第二个集合 —— 来自 GetDC/GetWindowDC 的"窗口 DC"（不要求覆盖整个屏幕）。
   录屏器抓的是它选定的那个窗口（WndToCapture），窗口不是全屏；预览阶段用的是 GetDC(NULL)，
   于是"预览被替换、录制不被替换"完全可以用这一点解释，不能据此断言"录制不走 BitBlt"。 */
void AddWindowDC(HDC dc)
{
    if (dc == nullptr)
        return;
    Lock();
    if (g_windowDCs.size() > 64)
        g_windowDCs.clear();
    g_windowDCs.insert(dc);
    Unlock();
}

bool IsWindowDC(HDC dc)
{
    bool found = false;
    Lock();
    found = g_windowDCs.find(dc) != g_windowDCs.end();
    Unlock();
    return found;
}

/* v0.6.58：冻结状态、底图 DC 与虚拟屏幕矩形必须在同一把锁下取到同一时刻的值。
   旧版 Hook_BitBlt 的决策在锁内，但未命中日志用的 g_frozen 是解锁后再读一次 ——
   重建窗口里会出现"决策时未冻结、日志却说冻结"，台架据此被误导过两次。
   本结构由 SnapFrozenLocked() 填充（调用方必须已持有 g_cs）。 */
struct FrozenSnap
{
    bool frozen;
    HDC  dc;
    int  vx;
    int  vy;
    int  vw;
    int  vh;
};

void SnapFrozenLocked(FrozenSnap* s)
{
    s->frozen = g_frozen;
    s->dc     = g_frozenDC;
    s->vx     = g_vx;
    s->vy     = g_vy;
    s->vw     = g_vw;
    s->vh     = g_vh;
}

void AddScreenDC(HDC dc)
{
    if (dc == nullptr)
        return;
    Lock();
    if (g_screenDCs.size() > 32)
        g_screenDCs.clear();      /* 防止长时间运行后句柄复用导致误判 */
    g_screenDCs.insert(dc);
    Unlock();
}

void RemoveScreenDC(HDC dc)
{
    if (dc == nullptr)
        return;
    Lock();
    g_screenDCs.erase(dc);
    Unlock();
}

bool IsScreenDC(HDC dc)
{
    bool found = false;
    Lock();
    found = g_screenDCs.find(dc) != g_screenDCs.end();
    Unlock();
    return found;
}

void ReleaseFrozen()
{
    Lock();
    if (g_frozenDC != nullptr)
    {
        if (g_frozenBmp != nullptr)
            DeleteObject(g_frozenBmp);
        DeleteDC(g_frozenDC);
    }
    g_frozenDC   = nullptr;
    g_frozenBmp  = nullptr;
    g_frozenBits = nullptr;
    g_frozen     = false;
    Unlock();
}
} /* namespace */

/* v0.6.51：判断一个窗口是否"实际上就是整块屏幕"。
   背景：厂商的 GDI 抓屏是 GetDC(某个全屏窗口)，不是 GetDC(NULL)；旧代码只在 hwnd==NULL 时
   把 DC 记成"屏幕 DC"，于是 IsScreenDC 永远返回 false，Hook_BitBlt 的替换分支一次都没进过。
   注意：hwnd == NULL 必须算"是屏幕"，否则 CaptureFreeze 自己取屏用的 GetDC(NULL) 都认不出来
   （v0.6.51 第一次改就栽在这，导致冻结全盘失效、回归挂 4 项）。 */
static bool IsScreenOrFullScreenWindow(HWND hwnd)
{
    if (hwnd == nullptr)
        return true;                      /* GetDC(NULL) = 整屏 */
    if (hwnd == GetDesktopWindow())
        return true;

    RECT rc = { 0, 0, 0, 0 };
    if (!GetWindowRect(hwnd, &rc))
        return false;

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0)
        return false;

    const int tol = 8;                    /* 全屏窗口常见 -8 这类边框偏移 */
    return (rc.left <= vx + tol) && (rc.top <= vy + tol) &&
           (rc.right >= vx + vw - tol) && (rc.bottom >= vy + vh - tol);
}

HDC WINAPI Hook_GetDC(HWND hwnd)
{
    if (!yzhook::TryEnterHook())
        return g_realGetDC(hwnd);
    HDC dc = g_realGetDC(hwnd);
    if (dc != nullptr && IsScreenOrFullScreenWindow(hwnd))
        AddScreenDC(dc);
    AddWindowDC(dc);
    yzhook::LeaveHook();
    return dc;
}

HDC WINAPI Hook_GetWindowDC(HWND hwnd)
{
    if (!yzhook::TryEnterHook())
        return g_realGetWindowDC(hwnd);
    HDC dc = g_realGetWindowDC(hwnd);
    if (dc != nullptr && IsScreenOrFullScreenWindow(hwnd))
        AddScreenDC(dc);
    AddWindowDC(dc);
    yzhook::LeaveHook();
    return dc;
}

HDC WINAPI Hook_CreateDCW(LPCWSTR driver, LPCWSTR device, LPCWSTR port, const DEVMODEW* mode)
{
    if (!yzhook::TryEnterHook())
        return g_realCreateDCW(driver, device, port, mode);
    HDC dc = g_realCreateDCW(driver, device, port, mode);
    if (dc != nullptr && driver != nullptr && _wcsicmp(driver, L"DISPLAY") == 0)
        AddScreenDC(dc);
    yzhook::LeaveHook();
    return dc;
}

int WINAPI Hook_ReleaseDC(HWND hwnd, HDC dc)
{
    if (!yzhook::TryEnterHook())
        return g_realReleaseDC(hwnd, dc);
    RemoveScreenDC(dc);
    int result = g_realReleaseDC(hwnd, dc);
    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_DeleteDC(HDC dc)
{
    if (!yzhook::TryEnterHook())
        return g_realDeleteDC(dc);
    RemoveScreenDC(dc);
    BOOL result = g_realDeleteDC(dc);
    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_BitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop)
{
    if (!yzhook::TryEnterHook())
        return g_realBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);

    BOOL result = FALSE;
    bool redirected = false;
    const bool srcIsWindowDC = IsWindowDC(hdcSrc);

    /* v0.6.58：一次锁内快照，决策与日志都用它。 */
    FrozenSnap snap;
    bool bySizeOnly = false;
    /* v0.6.56：冻结标志、底图 DC 与虚拟屏幕原点必须在同一把锁下取用。
       否则会和 CaptureFreeze() 里 ReleaseFrozen()→重建之间撕裂：钩子读到旧的 g_frozen=true
       与新的 g_frozenDC=nullptr，第三个条件不成立 ⇒ 明明两个判定位都是 1 却没被替换，
       现场看到的就是那条自相矛盾的"未替换"日志。 */
    Lock();
    SnapFrozenLocked(&snap);
    if (snap.frozen && snap.dc != nullptr)
    {
        const bool dcKnown = IsScreenDC(hdcSrc) || srcIsWindowDC;
        /* v0.6.57：整屏兜底。现场偶发缺陷（5 次样本 4 成 1 败）是**抓屏 DC 未登记**：
           抓屏进程缓存的 DC 若在装钩之前取得，就永远进不了登记表，之后整屏 BitBlt 全部命中不了。
           此时按"尺寸与位置都等于虚拟屏幕"认作抓屏，避免因为登记缺失整段失效。 */
        const bool sizeMatch = (cx == snap.vw && cy == snap.vh && x1 == snap.vx && y1 == snap.vy);
        if (dcKnown || sizeMatch)
        {
        bySizeOnly = (!dcKnown && sizeMatch);
        result = g_realBitBlt(hdc, x, y, cx, cy, snap.dc, x1 - snap.vx, y1 - snap.vy, rop);
        redirected = true;
        const LONG seq = InterlockedIncrement(&g_redirectCount);
        if (seq <= 5)
        {
            if (bySizeOnly)
                YZLOGI(L"[抓屏替换] BitBlt 按整屏尺寸兜底命中（DC 未登记，第 %d 次）：%dx%d @ %d,%d rop=0x%08X",
                       seq, cx, cy, x, y, (unsigned)rop);
            else
                YZLOGI(L"[抓屏替换] BitBlt 命中屏幕源（第 %d 次）：%dx%d @ %d,%d rop=0x%08X",
                       seq, cx, cy, x, y, (unsigned)rop);
        }
        }
    }
    Unlock();
    if (!redirected)
    {
        result = g_realBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
        /* v0.6.55：冻结期间"没被替换"的 BitBlt 也记前 5 次，用来区分
           "根本没调 BitBlt" 与 "调了但源 DC 没被认出来"。 */
        if (snap.frozen)
        {
            static LONG s_miss = 0;
            const LONG miss = InterlockedIncrement(&s_miss);
            if (miss <= 5)
                YZLOGI(L"[抓屏替换] 未替换的 BitBlt（第 %d 次）：%dx%d src=0x%p 窗口DC=%d 屏幕DC=%d 冻结=%d 底图DC=0x%p rop=0x%08X",
                       miss, cx, cy, hdcSrc, srcIsWindowDC ? 1 : 0, IsScreenDC(hdcSrc) ? 1 : 0,
                       snap.frozen ? 1 : 0, reinterpret_cast<void*>(snap.dc), (unsigned)rop);
        }
    }
    (void)redirected;

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_StretchBlt(HDC hdc, int x, int y, int cx, int cy,
                            HDC hdcSrc, int x1, int y1, int w1, int h1, DWORD rop)
{
    if (!yzhook::TryEnterHook())
        return g_realStretchBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, w1, h1, rop);

    BOOL result = FALSE;
    bool redirected = false;
    FrozenSnap snap;
    Lock();
    SnapFrozenLocked(&snap);
    if (snap.frozen && snap.dc != nullptr)
    {
        const bool dcKnown = IsScreenDC(hdcSrc) || IsWindowDC(hdcSrc);
        const bool sizeMatch = (cx == snap.vw && cy == snap.vh);
        if (dcKnown || sizeMatch)
        {
            result = g_realStretchBlt(hdc, x, y, cx, cy, snap.dc, x1 - snap.vx, y1 - snap.vy, w1, h1, rop);
            redirected = true;
            const LONG seqS = InterlockedIncrement(&g_redirectCount);
            if (seqS <= 5)
                YZLOGI(L"[抓屏替换] StretchBlt 命中屏幕源（第 %d 次）：%dx%d rop=0x%08X%s", seqS, cx, cy, (unsigned)rop,
                       dcKnown ? L"" : L"（按整屏尺寸兜底）");
        }
    }
    Unlock();
    if (!redirected)
        result = g_realStretchBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, w1, h1, rop);

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_PrintWindow(HWND hwnd, HDC hdc, UINT flags)
{
    if (!yzhook::TryEnterHook())
        return g_realPrintWindow(hwnd, hdc, flags);

    BOOL result = FALSE;
    bool redirected = false;
    FrozenSnap snap;
    Lock();
    SnapFrozenLocked(&snap);
    if (snap.frozen && snap.dc != nullptr)
    {
        result = g_realBitBlt(hdc, 0, 0, snap.vw, snap.vh, snap.dc, 0, 0, SRCCOPY);
        redirected = true;
        InterlockedIncrement(&g_redirectCount);
    }
    Unlock();
    if (!redirected)
        result = g_realPrintWindow(hwnd, hdc, flags);

    yzhook::LeaveHook();
    return result;
}

/* v0.6.59：GetDCEx 拿到的 DC 也登记成"窗口 DC"。
   之前只登记 GetDC/GetWindowDC，用 GetDCEx 取 DC 的代码（多见于 2000 年代的老模块）漏了。 */
HDC WINAPI Hook_GetDCEx(HWND hwnd, HRGN hrgnClip, DWORD flags)
{
    if (!yzhook::TryEnterHook())
        return g_realGetDCEx(hwnd, hrgnClip, flags);
    HDC dc = g_realGetDCEx(hwnd, hrgnClip, flags);
    AddWindowDC(dc);
    yzhook::LeaveHook();
    return dc;
}

/* v0.6.59：GetDIBits 观测。这是唯一没被覆盖的 GDI 抓屏原语，AdCap/ExdPaint/ScreenRecoder 都导入它。
   这一版**只记录、不改变行为**——先用数据判断"课堂监视那条路到底用不用它"，再决定要不要冻结它。
   记录 hdc 是否已登记、目标位图尺寸、以及当时冻结与否，前 5 次。 */
int WINAPI Hook_GetDIBits(HDC hdc, HBITMAP hbm, UINT start, UINT lines,
                          LPVOID bits, LPBITMAPINFO bmi, UINT usage)
{
    if (!yzhook::TryEnterHook())
        return g_realGetDIBits(hdc, hbm, start, lines, bits, bmi, usage);

    const int result = g_realGetDIBits(hdc, hbm, start, lines, bits, bmi, usage);

    static LONG s_seen = 0;
    const LONG n = InterlockedIncrement(&s_seen);
    if (n <= 5)
    {
        BITMAP bm;
        ZeroMemory(&bm, sizeof(bm));
        if (hbm != nullptr)
            GetObjectW(hbm, sizeof(bm), &bm);
        FrozenSnap snap;
        Lock();
        SnapFrozenLocked(&snap);
        Unlock();
        YZLOGI(L"[抓屏观测] GetDIBits 第 %d 次：hdc=0x%p hbm=0x%p 位图=%dx%dx%dbpp start=%u lines=%u 窗口DC=%d 屏幕DC=%d 冻结=%d",
               n, hdc, hbm, (int)bm.bmWidth, (int)bm.bmHeight, (int)bm.bmBitsPixel,
               start, lines, IsWindowDC(hdc) ? 1 : 0, IsScreenDC(hdc) ? 1 : 0, snap.frozen ? 1 : 0);
    }

    yzhook::LeaveHook();
    return result;
}

namespace yzhook
{
bool CaptureHooksInstall(bool enableNow)
{
    if (!g_csInit)
    {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
    if (g_hooksReady)
        return true;

    bool ok = true;

    /* 两阶段安装：先只创建（enableNow=false），等原始函数指针全部取回后再整组启用。
       反过来的话，钩子在 g_real* 仍为空指针时就已经生效，目标进程此时调用
       GetDC/BitBlt 就会跳进 detour 里的空指针，直接以 0xC0000005 崩掉宿主进程
       （实测崩溃点：错误模块 unknown、错误偏移量 0x00000000）。 */
    ok = HookAttach("capture", L"user32.dll", "GetDC", reinterpret_cast<void*>(&Hook_GetDC), false) && ok;
    ok = HookAttach("capture", L"user32.dll", "GetWindowDC", reinterpret_cast<void*>(&Hook_GetWindowDC), false) && ok;
    ok = HookAttach("capture", L"user32.dll", "ReleaseDC", reinterpret_cast<void*>(&Hook_ReleaseDC), false) && ok;
    ok = HookAttach("capture", L"user32.dll", "PrintWindow", reinterpret_cast<void*>(&Hook_PrintWindow), false) && ok;
    ok = HookAttach("capture", L"gdi32.dll", "CreateDCW", reinterpret_cast<void*>(&Hook_CreateDCW), false) && ok;
    ok = HookAttach("capture", L"gdi32.dll", "DeleteDC", reinterpret_cast<void*>(&Hook_DeleteDC), false) && ok;
    ok = HookAttach("capture", L"gdi32.dll", "BitBlt", reinterpret_cast<void*>(&Hook_BitBlt), false) && ok;
    ok = HookAttach("capture", L"gdi32.dll", "StretchBlt", reinterpret_cast<void*>(&Hook_StretchBlt), false) && ok;
    ok = HookAttach("capture", L"user32.dll", "GetDCEx", reinterpret_cast<void*>(&Hook_GetDCEx), false) && ok;
    ok = HookAttach("capture", L"gdi32.dll", "GetDIBits", reinterpret_cast<void*>(&Hook_GetDIBits), false) && ok;

    g_realGetDC       = reinterpret_cast<PFN_GetDC>(HookGetOriginal(reinterpret_cast<void*>(&Hook_GetDC)));
    g_realGetWindowDC = reinterpret_cast<PFN_GetWindowDC>(HookGetOriginal(reinterpret_cast<void*>(&Hook_GetWindowDC)));
    g_realReleaseDC   = reinterpret_cast<PFN_ReleaseDC>(HookGetOriginal(reinterpret_cast<void*>(&Hook_ReleaseDC)));
    g_realCreateDCW   = reinterpret_cast<PFN_CreateDCW>(HookGetOriginal(reinterpret_cast<void*>(&Hook_CreateDCW)));
    g_realDeleteDC    = reinterpret_cast<PFN_DeleteDC>(HookGetOriginal(reinterpret_cast<void*>(&Hook_DeleteDC)));
    g_realBitBlt      = reinterpret_cast<PFN_BitBlt>(HookGetOriginal(reinterpret_cast<void*>(&Hook_BitBlt)));
    g_realStretchBlt  = reinterpret_cast<PFN_StretchBlt>(HookGetOriginal(reinterpret_cast<void*>(&Hook_StretchBlt)));
    g_realPrintWindow = reinterpret_cast<PFN_PrintWindow>(HookGetOriginal(reinterpret_cast<void*>(&Hook_PrintWindow)));
    g_realGetDCEx     = reinterpret_cast<PFN_GetDCEx>(HookGetOriginal(reinterpret_cast<void*>(&Hook_GetDCEx)));
    g_realGetDIBits   = reinterpret_cast<PFN_GetDIBits>(HookGetOriginal(reinterpret_cast<void*>(&Hook_GetDIBits)));

    /* 只要有一个原指针没拿到，整组就保持停用：宁可功能不生效，也不能让任何
       detour 在空指针上被调用（Hook_PrintWindow 还会用到 g_realBitBlt，
       所以这里必须逐个检查，不能只查"关键"的两个）。 */
    if (g_realGetDC == nullptr || g_realGetWindowDC == nullptr || g_realReleaseDC == nullptr ||
        g_realCreateDCW == nullptr || g_realDeleteDC == nullptr || g_realBitBlt == nullptr ||
        g_realStretchBlt == nullptr || g_realPrintWindow == nullptr ||
        g_realGetDCEx == nullptr || g_realGetDIBits == nullptr)
    {
        YZLOGE(L"CaptureHooksInstall: 原始函数指针不完整，采集 hook 保持停用");
        return false;
    }

    if (enableNow && !HookSetGroupEnabled("capture", true))
    {
        YZLOGE(L"CaptureHooksInstall: 启用采集 hook 失败");
        return false;
    }

    g_hooksReady = true;
    YZLOGI(L"CaptureHooksInstall: 完成 (enableNow=%d)", enableNow ? 1 : 0);
    return ok;
}

void CaptureHooksShutdown()
{
    ReleaseFrozen();
    g_hooksReady = false;
}

bool CaptureFreeze()
{
    if (g_frozen)
        return true;
    if (g_realGetDC == nullptr || g_realBitBlt == nullptr)
    {
        /* v0.6.54：原来是静默返回，现场只看到"DXGI 冻结: 已开启"却查不出为什么没有底图。 */
        YZLOGI(L"CaptureFreeze: 钩子原指针未就绪（GetDC=%p BitBlt=%p），本次跳过",
               reinterpret_cast<void*>(g_realGetDC), reinterpret_cast<void*>(g_realBitBlt));
        return false;
    }

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0)
    {
        YZLOGE(L"CaptureFreeze: 虚拟屏幕尺寸非法（%d,%d %dx%d）", vx, vy, vw, vh);
        return false;
    }

    HDC screen = g_realGetDC(nullptr);
    if (screen == nullptr)
    {
        YZLOGE(L"CaptureFreeze: GetDC(NULL) 失败（err=%u）—— 会话无屏幕访问时会出现", GetLastError());
        return false;
    }

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = vw;
    bmi.bmiHeader.biHeight      = -vh;     /* 自上而下 */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp == nullptr)
    {
        g_realReleaseDC(nullptr, screen);
        YZLOGE(L"CaptureFreeze: CreateDIBSection 失败");
        return false;
    }

    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, bmp);
    BOOL copied = g_realBitBlt(mem, 0, 0, vw, vh, screen, vx, vy, SRCCOPY);
    SelectObject(mem, old);

    if (!copied)
    {
        DeleteObject(bmp);
        DeleteDC(mem);
        g_realReleaseDC(nullptr, screen);
        YZLOGE(L"CaptureFreeze: 抓取屏幕失败");
        return false;
    }

    Lock();
    ReleaseFrozen();
    g_frozenDC   = mem;
    g_frozenBmp  = bmp;
    g_frozenBits = bits;
    g_vx = vx;
    g_vy = vy;
    g_vw = vw;
    g_vh = vh;
    g_frozen = true;
    Unlock();

    g_realReleaseDC(nullptr, screen);

    /* v0.6.62：冻结底图的内容统计。
       现场出现过"教师端监视直接变黑"——合理怀疑底图抓到的是一张黑屏（教师锁屏、
       或广播窗口还在等待时屏幕本来就是黑的）。这里只统计并记录，不改变行为；
       近黑占比接近 100% 时给一条 WARN，提示"该先让屏幕显示正常内容再开防监视"。 */
    unsigned long avgLum  = 0;
    unsigned      nearBlackPct = 0;
    if (bits != nullptr && vw > 0 && vh > 0)
    {
        const unsigned char* px = static_cast<const unsigned char*>(bits);
        const size_t stride = static_cast<size_t>(vw) * 4;
        const size_t total  = static_cast<size_t>(vw) * static_cast<size_t>(vh);
        const size_t step   = (total > 200000) ? (total / 200000) : 1;   /* 抽样上限约 20 万点 */
        unsigned long long sum = 0;
        size_t counted = 0;
        size_t black   = 0;
        for (size_t idx = 0; idx < total; idx += step)
        {
            const unsigned char* q = px + (idx / static_cast<size_t>(vw)) * stride +
                                          (idx % static_cast<size_t>(vw)) * 4;
            const unsigned lum = (unsigned)q[2] * 29u / 100u +
                                 (unsigned)q[1] * 59u / 100u +
                                 (unsigned)q[0] * 11u / 100u;
            sum += lum;
            if (lum < 16u)
                black++;
            counted++;
        }
        if (counted > 0)
        {
            avgLum       = (unsigned long)(sum / counted);
            nearBlackPct = (unsigned)(black * 100ull / counted);
        }
    }
    YZLOGI(L"已冻结画面: 虚拟屏幕 %d,%d %dx%d 平均亮度=%lu 近黑占比=%u%%", vx, vy, vw, vh, avgLum, nearBlackPct);
    if (nearBlackPct >= 95u)
        YZLOGW(L"冻结底图几乎全黑（近黑占比 %u%%）：抓底图时屏幕本身可能就是黑的（教师锁屏/广播等待），"
               L"教师端监视会看到黑屏。想让掩护更像「没被动过」，先让屏幕显示正常内容再开防监视。", nearBlackPct);
    SendLogToHost(YZ_LOG_INFO, L"防监视已开启：教师端将看到固定画面");
    return true;
}

void CaptureUnfreeze()
{
    ReleaseFrozen();
    YZLOGI(L"已恢复实时画面");
}

bool CaptureIsFrozen()
{
    return g_frozen;
}

DWORD CaptureRedirectCount()
{
    return static_cast<DWORD>(g_redirectCount);
}
} /* namespace yzhook */

