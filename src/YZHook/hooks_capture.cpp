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

PFN_GetDC       g_realGetDC       = nullptr;
PFN_GetWindowDC g_realGetWindowDC = nullptr;
PFN_ReleaseDC   g_realReleaseDC   = nullptr;
PFN_CreateDCW   g_realCreateDCW   = nullptr;
PFN_DeleteDC    g_realDeleteDC    = nullptr;
PFN_BitBlt      g_realBitBlt      = nullptr;
PFN_StretchBlt  g_realStretchBlt  = nullptr;
PFN_PrintWindow g_realPrintWindow = nullptr;

CRITICAL_SECTION g_cs;
bool             g_csInit = false;
std::set<HDC>    g_screenDCs;

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

HDC WINAPI Hook_GetDC(HWND hwnd)
{
    if (!yzhook::TryEnterHook())
        return g_realGetDC(hwnd);
    HDC dc = g_realGetDC(hwnd);
    if (hwnd == nullptr && dc != nullptr)
        AddScreenDC(dc);
    yzhook::LeaveHook();
    return dc;
}

HDC WINAPI Hook_GetWindowDC(HWND hwnd)
{
    if (!yzhook::TryEnterHook())
        return g_realGetWindowDC(hwnd);
    HDC dc = g_realGetWindowDC(hwnd);
    if (dc != nullptr && (hwnd == nullptr || hwnd == GetDesktopWindow()))
        AddScreenDC(dc);
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
    if (g_frozen && IsScreenDC(hdcSrc) && g_frozenDC != nullptr)
    {
        result = g_realBitBlt(hdc, x, y, cx, cy, g_frozenDC, x1 - g_vx, y1 - g_vy, rop);
        redirected = true;
        InterlockedIncrement(&g_redirectCount);
    }
    else
    {
        result = g_realBitBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, rop);
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
    if (g_frozen && IsScreenDC(hdcSrc) && g_frozenDC != nullptr)
    {
        result = g_realStretchBlt(hdc, x, y, cx, cy, g_frozenDC, x1 - g_vx, y1 - g_vy, w1, h1, rop);
        InterlockedIncrement(&g_redirectCount);
    }
    else
    {
        result = g_realStretchBlt(hdc, x, y, cx, cy, hdcSrc, x1, y1, w1, h1, rop);
    }

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_PrintWindow(HWND hwnd, HDC hdc, UINT flags)
{
    if (!yzhook::TryEnterHook())
        return g_realPrintWindow(hwnd, hdc, flags);

    BOOL result = FALSE;
    if (g_frozen && g_frozenDC != nullptr)
    {
        result = g_realBitBlt(hdc, 0, 0, g_vw, g_vh, g_frozenDC, 0, 0, SRCCOPY);
        InterlockedIncrement(&g_redirectCount);
    }
    else
    {
        result = g_realPrintWindow(hwnd, hdc, flags);
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

    g_realGetDC       = reinterpret_cast<PFN_GetDC>(HookGetOriginal(reinterpret_cast<void*>(&Hook_GetDC)));
    g_realGetWindowDC = reinterpret_cast<PFN_GetWindowDC>(HookGetOriginal(reinterpret_cast<void*>(&Hook_GetWindowDC)));
    g_realReleaseDC   = reinterpret_cast<PFN_ReleaseDC>(HookGetOriginal(reinterpret_cast<void*>(&Hook_ReleaseDC)));
    g_realCreateDCW   = reinterpret_cast<PFN_CreateDCW>(HookGetOriginal(reinterpret_cast<void*>(&Hook_CreateDCW)));
    g_realDeleteDC    = reinterpret_cast<PFN_DeleteDC>(HookGetOriginal(reinterpret_cast<void*>(&Hook_DeleteDC)));
    g_realBitBlt      = reinterpret_cast<PFN_BitBlt>(HookGetOriginal(reinterpret_cast<void*>(&Hook_BitBlt)));
    g_realStretchBlt  = reinterpret_cast<PFN_StretchBlt>(HookGetOriginal(reinterpret_cast<void*>(&Hook_StretchBlt)));
    g_realPrintWindow = reinterpret_cast<PFN_PrintWindow>(HookGetOriginal(reinterpret_cast<void*>(&Hook_PrintWindow)));

    /* 只要有一个原指针没拿到，整组就保持停用：宁可功能不生效，也不能让任何
       detour 在空指针上被调用（Hook_PrintWindow 还会用到 g_realBitBlt，
       所以这里必须逐个检查，不能只查"关键"的两个）。 */
    if (g_realGetDC == nullptr || g_realGetWindowDC == nullptr || g_realReleaseDC == nullptr ||
        g_realCreateDCW == nullptr || g_realDeleteDC == nullptr || g_realBitBlt == nullptr ||
        g_realStretchBlt == nullptr || g_realPrintWindow == nullptr)
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
        return false;

    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0)
        return false;

    HDC screen = g_realGetDC(nullptr);
    if (screen == nullptr)
        return false;

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

    YZLOGI(L"已冻结画面: 虚拟屏幕 %d,%d %dx%d", vx, vy, vw, vh);
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

