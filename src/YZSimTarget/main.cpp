//
// YZSimTarget.exe —— 模拟远志学生端的测试目标（仅用于本地验证 YZTrainer）。
//
// 模拟行为：
//   * 全屏、无边框、置顶的“广播画面”窗口，内容持续变化
//   * 每 3 秒重新把自己变回全屏（模拟学生端对抗窗口化）
//   * 安装 WH_KEYBOARD_LL 屏蔽除 Ctrl+Alt+F9..F12 以外的按键
//   * ClipCursor 把鼠标锁在屏幕内
//   * 尝试 RegisterHotKey(Ctrl+Alt+F9) 抢占热键
//   * 每 500ms 用 BitBlt 抓一帧全屏并记录哈希到 sim_capture.csv（用于验证防监视冻结）
//   * --exam 参数额外创建一个标题含“考试”的窗口，用于验证考试模式自动停用
//
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../../src/common/yz_util.h"

namespace
{
const wchar_t* const kSimClass = L"YZSimBroadcastWnd";
HWND  g_main = nullptr;
HWND  g_exam = nullptr;
HHOOK g_keyHook = nullptr;
int   g_frame = 0;
bool  g_examRequested = false;
HBITMAP g_captureBmp = nullptr;
void*   g_captureBits = nullptr;
int     g_captureW = 0;
int     g_captureH = 0;
HDC     g_captureDC = nullptr;
unsigned long long g_lastHash = 0;
std::wstring g_exeDir;

void AppendFile(const std::wstring& path, const std::wstring& line)
{
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::string utf8 = yz::WideToUtf8(line + L"\r\n");
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
}

std::wstring StateFile()   { return yz::JoinPath(g_exeDir, L"sim_state.txt"); }
std::wstring CaptureFile() { return yz::JoinPath(g_exeDir, L"sim_capture.csv"); }

void DumpState(const wchar_t* reason)
{
    if (g_main == nullptr)
        return;
    RECT rc;
    GetWindowRect(g_main, &rc);
    LONG style = GetWindowLongW(g_main, GWL_STYLE);
    LONG ex    = GetWindowLongW(g_main, GWL_EXSTYLE);
    int monW = GetSystemMetrics(SM_CXSCREEN);
    int monH = GetSystemMetrics(SM_CYSCREEN);
    std::wstring line = yz::Format(
        L"%s tick=%u style=0x%08X ex=0x%08X rect=%d,%d,%d,%d monitor=%dx%d fullscreen=%d topmost=%d",
        reason, GetTickCount(), style, ex, rc.left, rc.top, rc.right, rc.bottom,
        monW, monH,
        (rc.left <= 0 && rc.top <= 0 && (rc.right - rc.left) >= monW && (rc.bottom - rc.top) >= monH) ? 1 : 0,
        (ex & WS_EX_TOPMOST) ? 1 : 0);
    /* 状态文件用覆盖写，方便测试脚本直接读取最新状态 */
    HANDLE h = CreateFileW(StateFile().c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        std::string utf8 = yz::WideToUtf8(line + L"\r\n");
        DWORD written = 0;
        WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        CloseHandle(h);
    }
}

void CaptureFrame()
{
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vw <= 0 || vh <= 0)
        return;

    HDC screen = GetDC(nullptr);
    if (screen == nullptr)
        return;

    if (g_captureDC == nullptr || g_captureW != vw || g_captureH != vh)
    {
        if (g_captureDC != nullptr)
        {
            DeleteDC(g_captureDC);
            DeleteObject(g_captureBmp);
            g_captureDC = nullptr;
        }
        BITMAPINFO bmi;
        ZeroMemory(&bmi, sizeof(bmi));
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = vw;
        bmi.bmiHeader.biHeight = -vh;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        g_captureBmp = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &g_captureBits, nullptr, 0);
        g_captureDC = CreateCompatibleDC(screen);
        if (g_captureBmp != nullptr)
            SelectObject(g_captureDC, g_captureBmp);
        g_captureW = vw;
        g_captureH = vh;
    }

    BOOL ok = BitBlt(g_captureDC, 0, 0, vw, vh, screen, vx, vy, SRCCOPY);
    ReleaseDC(nullptr, screen);
    if (!ok || g_captureBits == nullptr)
        return;

    unsigned long long hash = yz::Fnv1a64(g_captureBits, static_cast<size_t>(vw) * vh * 4);
    g_lastHash = hash;
    AppendFile(CaptureFile(), yz::Format(L"%u,%llu", GetTickCount(), hash));
}

LRESULT CALLBACK KeyHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION)
    {
        KBDLLHOOKSTRUCT* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool alt  = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        bool isF9ToF12 = kb->vkCode >= VK_F9 && kb->vkCode <= VK_F12;
        if (!(ctrl && alt && isF9ToF12))
        {
            /* 模拟远志的输入封锁 */
            return 1;
        }
    }
    return CallNextHookEx(g_keyHook, code, wParam, lParam);
}

void DrawContent(HDC dc, const RECT& rc)
{
    HBRUSH bg = CreateSolidBrush(RGB(20, 24, 32));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    int x = (g_frame * 7) % (rc.right - 120);
    HBRUSH box = CreateSolidBrush(RGB(64, 160, 255));
    RECT r;
    r.left = 40 + x; r.top = 80; r.right = r.left + 120; r.bottom = 180;
    FillRect(dc, &r, box);
    DeleteObject(box);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    std::wstring text = yz::Format(L"YZSimTarget 模拟全屏广播  frame=%d", g_frame);
    TextOutW(dc, 40, 220, text.c_str(), static_cast<int>(text.size()));
    TextOutW(dc, 40, 250, L"（这是模拟窗口，不是真实广播）", -1);

    int monW = GetSystemMetrics(SM_CXSCREEN);
    int monH = GetSystemMetrics(SM_CYSCREEN);
    RECT line;
    line.left = 0; line.top = monH - 4; line.right = (monW * (g_frame % 100)) / 100; line.bottom = monH;
    HBRUSH bar = CreateSolidBrush(RGB(255, 96, 96));
    FillRect(dc, &line, bar);
    DeleteObject(bar);
}

void MakeFullscreen()
{
    if (g_main == nullptr)
        return;
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    SetWindowLongW(g_main, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongW(g_main, GWL_EXSTYLE, WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    SetWindowPos(g_main, HWND_TOPMOST, 0, 0, w, h, SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    ClipCursor(nullptr);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_TIMER:
        if (wParam == 1)
        {
            g_frame++;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        else if (wParam == 2)
        {
            MakeFullscreen();
            DumpState(L"timer-refullscreen");
        }
        else if (wParam == 3)
        {
            CaptureFrame();
        }
        else if (wParam == 4)
        {
            DumpState(L"timer-state");
        }
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc;
            GetClientRect(hwnd, &rc);
            DrawContent(dc, rc);
            EndPaint(hwnd, &ps);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK ExamProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(120, 120, 120));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        EndPaint(hwnd, &ps);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} /* namespace */

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR cmdLine, int)
{
    g_exeDir = yz::GetExeDir();
    g_examRequested = (cmdLine != nullptr) && (wcsstr(cmdLine, L"--exam") != nullptr);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = kSimClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kSimClass,
                             L"YZSimTarget 模拟广播", WS_POPUP | WS_VISIBLE,
                             0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                             nullptr, nullptr, hinst, nullptr);
    if (g_main == nullptr)
        return 1;

    MakeFullscreen();
    ShowWindow(g_main, SW_SHOW);
    UpdateWindow(g_main);

    SetTimer(g_main, 1, 100, nullptr);    /* 动画 */
    SetTimer(g_main, 2, 3000, nullptr);   /* 定期抢回全屏 */
    SetTimer(g_main, 3, 500, nullptr);    /* 抓帧 */
    SetTimer(g_main, 4, 1000, nullptr);   /* 状态落盘 */

    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHookProc, nullptr, 0);
    ClipCursor(nullptr);

    BOOL hotkeyOk = RegisterHotKey(g_main, 1, MOD_CONTROL | MOD_ALT, VK_F9);
    AppendFile(yz::JoinPath(g_exeDir, L"sim_log.txt"),
               yz::Format(L"start tick=%u keyHook=%p registerHotKey(F9)=%d",
                          GetTickCount(), g_keyHook, hotkeyOk ? 1 : 0));

    if (g_examRequested)
    {
        WNDCLASSEXW ec;
        ZeroMemory(&ec, sizeof(ec));
        ec.cbSize        = sizeof(ec);
        ec.lpfnWndProc   = ExamProc;
        ec.hInstance     = hinst;
        ec.lpszClassName = L"YZSimExamWnd";
        ec.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        RegisterClassExW(&ec);
        g_exam = CreateWindowExW(WS_EX_TOPMOST, L"YZSimExamWnd", L"模拟考试模式窗口",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 100, 100, 420, 240, nullptr, nullptr, hinst, nullptr);
        AppendFile(yz::JoinPath(g_exeDir, L"sim_log.txt"), L"考试模拟窗口已创建");
    }

    DumpState(L"startup");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_keyHook != nullptr)
        UnhookWindowsHookEx(g_keyHook);
    ClipCursor(nullptr);
    return 0;
}


