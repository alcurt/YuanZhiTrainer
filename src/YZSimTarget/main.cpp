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
//   * --exec 参数在启动 4 秒后拉起一个子进程（cmd.exe /c echo …），
//     用于验证“教师端远程执行审计”能在学生进程里记录到 CreateProcess
//
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
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
bool  g_execRequested = false;
/* --yank [毫秒]：周期性把光标移到 (400,300)、随后拽回 (0,0)，模拟机房"全屏广播/锁键鼠时鼠标能动
   几百毫秒就被拽回左上角"的现象（2026-10-15 用户描述）。用于**验证输入取证**（YZHook 的
   [输入取证] 只记录不拦截）：学生端/模拟目标是被允许挂钩子的宿主，所以必须用它来验。 */
bool  g_yankRequested = false;
UINT  g_yankIntervalMs = 500;
bool  g_yankAway = true;
/* 兜底（2026-10-15 用户提问："模拟目标卡住过 200 帧还没结束控制，有兜底吗"）：
   以前**没有任何自我终止机制** —— 跑起来只能靠关窗或被强杀，卡住就得等人手动处理或等红线。
   现在：① 活过 g_maxSeconds（默认 600 秒，--max-seconds 可改，0=不限）自己退出；
        ② 若环境变量 YZ_SIM_PARENT_PID 指向的进程已经消失，立即退出（防止父进程崩了留孤儿）。 */
UINT  g_maxSeconds = 600;
DWORD g_parentPid  = 0;
ULONGLONG g_startTick = 0;      /* 进程启动时刻：时限从**启动**算，不从第一次定时器触发算 */
/* --load <dll>：4 秒后再加载指定 DLL，用来验证"远志的钩子模块晚于注入出现"时
   主动拔钩的重试（NativeUnhookTick）会不会真的再试一次。模块名必须与被查的名字
   一致（例如把 ExdHooks.dll 复制出来单独放）。 */
std::wstring g_lateLoadDll;
/* --dup [帧数]：真的建一次 DXGI 桌面复制并取若干帧（用于验证 YZHook 的 DXGI 取证钩子）。
   只为取证而用，所以 dxgi/d3d11 一律动态加载，不给模拟目标增加静态依赖。 */
int          g_dupFrames = 0;
/* --hook / --hook-thread / --hook-message：像厂商那样"自己装钩子 + 把 NULL 当生死判据"（回归替身） */
bool g_hookMode        = false;
bool g_hookThreadMode  = false;
bool g_hookMessageMode = false;   /* 额外装一个 WH_GETMESSAGE，验证 BlockMessageHooks 开关 */
HHOOK g_msgHook        = nullptr;
int  g_exitCode        = 0;     /* 非 0 = 模拟客户端"自杀"退出，回归据此判 FAIL */
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

/* 模拟"教师端远程执行命令"：在学生进程里跑一次 CreateProcess，
   审计 hook 应该把它记进 remote-exec.log（origin=remote） */
/* --load：4 秒后加载一个 DLL，模拟"注入已经发生、远志的钩子模块随后才被加载"。
   主程序侧应当在此之后 2 秒内再试一次主动拔钩（NativeUnhookTick）。 */
void LoadLateDllOnce()
{
    if (g_lateLoadDll.empty())
        return;
    HMODULE mod = LoadLibraryW(g_lateLoadDll.c_str());
    AppendFile(yz::JoinPath(g_exeDir, L"sim_log.txt"),
               yz::Format(L"late-load %s -> %p err=%u", g_lateLoadDll.c_str(),
                          reinterpret_cast<void*>(mod), mod ? 0u : GetLastError()));
}

/* --dup：真的建一次桌面复制并取 N 帧，把结果写进 sim_log.txt。
   用途：让回归能证明"YZHook 的 DXGI 取证钩子确实接管了厂商侧的 DuplicateOutput 与
   AcquireNextFrame"——模拟目标在这里扮演厂商那个采集线程。 */
/* 抓屏子进程替身（v0.6.32）：机房实测厂商是"学生端亲手拉起
   rundll32.exe <...>\KsFiles\ExdDtDup.dll,RunCapture 0"。这里照抄这个形状：拉起自己
   （--dup 模式）并让命令行带上同一个模块路径标记，好让 YZHook 的抓屏子进程识别命中
   （模拟目标自己就是被注入的那个"学生端"）。*/
static bool g_spawnCaptureChild = false;
static DWORD g_dupDelayMs = 8000;   /* --dup-delay <秒>：探针推后（v0.6.33 验证用） */

void SpawnCaptureChildOnce()
{
    wchar_t self[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring child = yz::Format(L"\"%s\" --dup 60 \"C:\\Program Files (x86)\\GZYZ\\"
                                    L"YZinfo Multimedia teaching softwareV9.0 Student\\"
                                    L"KsFiles\\ExdDtDup.dll\",RunCapture 0", self);
    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));
    /* v0.6.32 本机验证用：让子进程额外加载一个**文件名就叫 ExdDtDup.dll** 的模块，
       这样"采集栈监控"这条**身份无关**的触发路径能在本机响（第一条 CreateProcessW 触发
       本机测不出来：仓库路径里带 Yuanzhi/Teaching，会被判成 self 因而不镜像到宿主）。 */
    {
        wchar_t envDll[512] = { 0 };
        if (GetEnvironmentVariableW(L"YZ_TEST_LOAD_DLL", envDll, 512) > 0)
            child += yz::Format(L" --load \"%s\"", envDll);
    }
    const BOOL ok = CreateProcessW(nullptr, &child[0], nullptr, nullptr, FALSE, 0,
                                   nullptr, nullptr, &si, &pi);
    AppendFile(yz::JoinPath(g_exeDir, L"sim_log.txt"),
               yz::Format(L"spawn-child: 已拉起抓屏子进程替身 ok=%d pid=%u",
                          ok ? 1 : 0, ok ? pi.dwProcessId : 0));
    if (ok)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

void RunDupProbe()
{
    const std::wstring logPath = yz::JoinPath(g_exeDir, L"sim_log.txt");
    HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
    HMODULE dxgi  = GetModuleHandleW(L"dxgi.dll");
    if (dxgi == nullptr)
        dxgi = LoadLibraryW(L"dxgi.dll");
    if (d3d11 == nullptr || dxgi == nullptr)
    {
        AppendFile(logPath, yz::Format(L"dup: 加载 d3d11/dxgi 失败 err=%u", GetLastError()));
        return;
    }

    typedef HRESULT (WINAPI *PFN_D3D11CreateDevice)(void*, UINT, void*, UINT, const void*, UINT, UINT,
                                                    void**, UINT*, void**);
    PFN_D3D11CreateDevice createDevice =
        reinterpret_cast<PFN_D3D11CreateDevice>(GetProcAddress(d3d11, "D3D11CreateDevice"));
    if (createDevice == nullptr)
    {
        AppendFile(logPath, L"dup: 取 D3D11CreateDevice 失败");
        return;
    }

    ID3D11Device*        dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    UINT level = 0;
    HRESULT hr = createDevice(nullptr, 1 /*D3D_DRIVER_TYPE_HARDWARE*/, nullptr, 0, nullptr, 0,
                              7 /*D3D11_SDK_VERSION*/, reinterpret_cast<void**>(&dev), &level,
                              reinterpret_cast<void**>(&ctx));
    if (FAILED(hr) || dev == nullptr)
    {
        AppendFile(logPath, yz::Format(L"dup: D3D11CreateDevice hr=0x%08X", static_cast<unsigned>(hr)));
        return;
    }

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIOutput*  output  = nullptr;
    IDXGIOutput1* output1 = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    int frames = 0, timeouts = 0;

    hr = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev));
    if (SUCCEEDED(hr)) hr = dxgiDev->GetAdapter(&adapter);
    if (SUCCEEDED(hr)) hr = adapter->EnumOutputs(0, &output);
    if (SUCCEEDED(hr)) hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
    if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(dev, &dup);

    if (SUCCEEDED(hr) && dup != nullptr)
    {
        for (int i = 0; i < g_dupFrames; ++i)
        {
            DXGI_OUTDUPL_FRAME_INFO info;
            ZeroMemory(&info, sizeof(info));
            IDXGIResource* res = nullptr;
            const HRESULT fhr = dup->AcquireNextFrame(50, &info, &res);
            if (fhr == DXGI_ERROR_WAIT_TIMEOUT)
            {
                ++timeouts;
                continue;
            }
            if (FAILED(fhr))
            {
                AppendFile(logPath, yz::Format(L"dup: AcquireNextFrame 失败 hr=0x%08X（第 %d 次）",
                                               static_cast<unsigned>(fhr), i));
                break;
            }
            ++frames;
            if (res != nullptr)
                res->Release();
            dup->ReleaseFrame();
        }
        AppendFile(logPath, yz::Format(L"dup: 完成 取到帧=%d 超时=%d（请求 %d）", frames, timeouts, g_dupFrames));
        dup->Release();
    }
    else
    {
        AppendFile(logPath, yz::Format(L"dup: 没能建立桌面复制 hr=0x%08X（0x887A0022=已被别的客户端占用）",
                                       static_cast<unsigned>(hr)));
    }

    if (output1 != nullptr) output1->Release();
    if (output  != nullptr) output->Release();
    if (adapter != nullptr) adapter->Release();
    if (dxgiDev != nullptr) dxgiDev->Release();
    if (ctx != nullptr) ctx->Release();
    if (dev != nullptr) dev->Release();
}

/* WH_GETMESSAGE / WH_CALLWNDPROC 这类"消息层"钩子用的空过程。
   注意不能拿 KeyHookProc 顶替：消息钩子的 lParam 是 MSG*，KeyHookProc 会把它当
   KBDLLHOOKSTRUCT 读，然后几乎每条消息都 return 1（消息被丢掉），模拟目标自己就废了。 */
LRESULT CALLBACK MessageHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

/* --hook / --hook-thread / --hook-message 的实体：像厂商那样自己装钩子，并且**把返回 NULL
   当生死判据**。现场证据（2026-10-09）：v0.6.10 把远志的 SetWindowsHookEx 拦成 NULL 时
   Yistart.exe 8 毫秒内退出，守卫 4 秒后拉起 → 十秒级重启循环（电子点名窗口立不住）。
   v0.6.11 改成"用本 DLL 的空过程顶替、返回合法 HHOOK"后客户端不再重启。这里复现那条
   致命反应，让回归阶段八**真的能变红**：拿到 NULL 就退出码 3。
     --hook        键盘 LL，hMod = 本 exe 模块（与现场"厂商 DLL 传自己模块"同构）
     --hook-thread 键盘 WH_KEYBOARD，hMod = NULL + 本线程 tid（命中"回调反查模块"判据）
     --hook-message 额外再装一个 WH_GETMESSAGE（hMod = 本 exe 模块、限本线程），
                   用来验证 ini 项 BlockMessageHooks 的开关行为（阶段九）：
                   开关为 1 时它也该被换成空过程，为 0 时应原样放行。
   三者都要求模拟目标被放在**路径含 GZYZ** 的目录里，否则 ShouldBlockHook 会正确地放行
   （回归脚本就是这么摆的：先复制到 %TEMP%\GZYZ-yzsim\ 再跑，再等 8 秒装钩子——
   那时 YZHook 已经注进来了；启动时立刻装会走系统原生路径，第一版实测两边都不命中）。 */
void InstallSelfHookOrDie()
{
    if (!g_hookMode && !g_hookThreadMode && !g_hookMessageMode)
        return;

    if (g_hookMode || g_hookThreadMode)
    {
        HHOOK h = g_hookThreadMode
            ? SetWindowsHookExW(WH_KEYBOARD, KeyHookProc, nullptr, GetCurrentThreadId())
            : SetWindowsHookExA(WH_KEYBOARD_LL, KeyHookProc, GetModuleHandleW(nullptr), 0);
        AppendFile(yz::JoinPath(g_exeDir, L"sim_log.txt"),
                   yz::Format(L"hook-install mode=%s hhook=%p err=%u tick=%u",
                              g_hookThreadMode ? L"thread" : L"ll-with-hmod",
                              reinterpret_cast<void*>(h), GetLastError(), GetTickCount()));
        if (h == nullptr)
        {
            AppendFile(yz::JoinPath(g_exeDir, L"sim_log.txt"),
                       L"hook-install FAILED -> 模拟客户端据此自杀（现场：8 毫秒内进程消失）");
            g_exitCode = 3;
            PostQuitMessage(3);
            return;
        }
        g_keyHook = h;
    }

    if (g_hookMessageMode)
    {
        /* 消息层钩子：hMod 给本 exe 模块、线程限自己这条线程（跨进程的全局消息钩子必须由 DLL 装，
           exe 装不了；限本线程时合法，且同样会经过我们的 SetWindowsHookEx 钩子）。 */
        HHOOK hm = SetWindowsHookExW(WH_GETMESSAGE, MessageHookProc, GetModuleHandleW(nullptr),
                                     GetCurrentThreadId());
        AppendFile(yz::JoinPath(g_exeDir, L"sim_log.txt"),
                   yz::Format(L"hook-install mode=message-gettmsg hhook=%p err=%u tick=%u",
                              reinterpret_cast<void*>(hm), GetLastError(), GetTickCount()));
        if (hm == nullptr)
        {
            AppendFile(yz::JoinPath(g_exeDir, L"sim_log.txt"),
                       L"hook-install(消息层) FAILED -> 模拟客户端据此自杀");
            g_exitCode = 3;
            PostQuitMessage(3);
            return;
        }
        g_msgHook = hm;
    }
}

void SpawnChildOnce()
{
    wchar_t sysDir[MAX_PATH] = {0};
    if (GetSystemDirectoryW(sysDir, MAX_PATH) == 0)
        return;

    const std::wstring exe = yz::JoinPath(sysDir, L"cmd.exe");
    std::wstring cmdLine = L"cmd.exe /c echo yztrainer-audit";

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    const BOOL ok = CreateProcessW(exe.c_str(), &cmdLine[0], nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    AppendFile(yz::JoinPath(g_exeDir, L"sim_log.txt"),
               yz::Format(L"exec-child tick=%u ok=%d pid=%u exe=%s",
                          GetTickCount(), ok ? 1 : 0, ok ? pi.dwProcessId : 0, exe.c_str()));
    if (ok)
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
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

        else if (wParam == 11)
        {
            const ULONGLONG elapsed = (GetTickCount64() - g_startTick) / 1000ull;
            bool quit = false;
            if (g_maxSeconds > 0 && elapsed >= g_maxSeconds) quit = true;
            if (!quit && g_parentPid != 0)
            {
                HANDLE hParent = OpenProcess(SYNCHRONIZE, FALSE, g_parentPid);
                if (hParent == nullptr) quit = true;                       /* 父进程没了 */
                else { if (WaitForSingleObject(hParent, 0) == WAIT_OBJECT_0) quit = true; CloseHandle(hParent); }
            }
            if (quit) DestroyWindow(hwnd);                                  /* 走正常退出路径 */
        }
        else if (wParam == 10)
        {
            /* --yank：一次"移开"、一次"拽回"，交替进行 ⇒ 制造被测的那段"自由窗口" */
            /* v0.6.46：现场客户端是 SetCursorPos 与 mouse_event **并用**（机房日志里两者都是每秒 10 次），
               所以这里两个 API 都发，才能把 [输入取证]/[输入拦截] 里 mouse_event 那一路也覆盖到。 */
            if (g_yankAway) { SetCursorPos(400, 300); mouse_event(MOUSEEVENTF_MOVE, 1, 0, 0, 0); g_yankAway = false; }
            else            { SetCursorPos(0, 0);     mouse_event(MOUSEEVENTF_MOVE, 0, 1, 0, 0); g_yankAway = true; }
        }
        else if (wParam == 5)
        {
            KillTimer(hwnd, 5);
            SpawnChildOnce();
        }
        else if (wParam == 6)
        {
            KillTimer(hwnd, 6);
            LoadLateDllOnce();
        }
        else if (wParam == 7)
        {
            /* --fight：200ms 一次把无边框全屏抢回来，复现"客户端响应我们的改动、
               外部纠正怎么改都马上被顶回去"的横跳场景（验证反横跳闸门）。 */
            MakeFullscreen();
        }
        else if (wParam == 9)
        {
            /* --dup：延迟 3 秒跑一次桌面复制取证（定时器 8 已被 --hook 的延迟安装占用） */
            KillTimer(hwnd, 9);
            RunDupProbe();
    if (g_spawnCaptureChild)
        SpawnCaptureChildOnce();
        }
        else if (wParam == 8)
        {
            /* --hook / --hook-thread：**延迟**装自己的键盘钩子。
               为什么必须延迟：现场那次（2026-10-09）客户端是在 Trainer 已经注入之后
               才装钩子的；如果模拟目标在启动时（= 注入之前）就装，走的是系统原生路径，
               根本不会经过我们的 SetWindowsHookEx 钩子，阶段八就白测了
               ——第一版本就是这么写的，实测两边都不命中，日志里是原生 hhook。 */
            KillTimer(hwnd, 8);
            InstallSelfHookOrDie();
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
if (cmdLine != nullptr && wcsstr(cmdLine, L"--max-seconds") != nullptr)
{
    const int s = _wtoi(wcsstr(cmdLine, L"--max-seconds") + 13);
    if (s >= 0) g_maxSeconds = (UINT)s;
}
{
    wchar_t pidBuf[32] = { 0 };
    if (GetEnvironmentVariableW(L"YZ_SIM_PARENT_PID", pidBuf, 32) > 0)
        g_parentPid = (DWORD)_wtoi(pidBuf);
}
if (cmdLine != nullptr && wcsstr(cmdLine, L"--yank") != nullptr)
{
    g_yankRequested = true;
    if (const wchar_t* y = wcsstr(cmdLine, L"--yank "))
    {
        const UINT ms = (UINT)_wtoi(y + 7);
        if (ms >= 100) g_yankIntervalMs = ms;
    }
}
    g_execRequested = (cmdLine != nullptr) && (wcsstr(cmdLine, L"--exec") != nullptr);
    if (cmdLine != nullptr)
    {
        /* --spawn-child（v0.6.32）：本机替身，见 SpawnCaptureChildOnce()。
       注意**不在这里立刻拉起**：现场那次 spawn 发生在主程序注入之后，而我们本机要保证
       钩子已经装好、经过的是我们钩住的那次 CreateProcessW——所以挂到 3 秒定时器上。 */
    if (wcsstr(cmdLine, L"--spawn-child") != nullptr && wcsstr(cmdLine, L"--dup") == nullptr)
        g_spawnCaptureChild = true;

    const wchar_t* dupArg = wcsstr(cmdLine, L"--dup");
    /* --spawn-child 时没有 --dup，但下面"解析帧数"那段会 dupArg+5 后取值：
       给一个尾部空格的假参数，解析自然落回默认帧数（v0.6.32，否则空指针崩 0xC0000005）。 */
    if (dupArg == nullptr && g_spawnCaptureChild)
        dupArg = L"--dup ";
    /* --dup-delay <秒>（v0.6.33 验证用）：把桌面复制探针推迟到指定秒数再跑。
       为什么需要：验证"冻结画面"时，注入→配置→自检→武装这条链要 ~9~10 秒，而探针默认
       8 秒就跑，两者赛跑会让结果介于"全取到"和"全超时"之间（实测 31/29）。用这个参数
       把探针推到武装之后，才能拿到 0/60 这种干净对照。 */
    /* 结果写进文件作用域的 g_dupDelayMs（定时器设置在另一个作用域里） */
    {
        const wchar_t* d = wcsstr(cmdLine, L"--dup-delay ");
        if (d != nullptr)
        {
            const int sec = _wtoi(d + 12);
            if (sec > 0 && sec <= 600)
                g_dupDelayMs = static_cast<DWORD>(sec) * 1000;
        }
    }
        if (dupArg != nullptr || g_spawnCaptureChild)
        {
            g_dupFrames = 20;                       /* 默认取 20 帧 */
            const wchar_t* p = dupArg + 5;
            while (*p == L' ') ++p;
            const int n = _wtoi(p);
            if (n > 0 && n <= 1000)
                g_dupFrames = n;
        }
    }

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
    if (g_execRequested)
        SetTimer(g_main, 5, 4000, nullptr);   /* 一次性：模拟教师端远程执行 */

    /* 一次性：3 秒后跑一次 --dup 的桌面复制取证（此时注入与配置都已就绪） */
    if (g_dupFrames > 0)
        SetTimer(g_main, 9, g_dupDelayMs, nullptr);   /* 默认 8 秒；--dup-delay 可推后（见上） */
    if (g_yankRequested)
        SetTimer(g_main, 10, g_yankIntervalMs, nullptr);   /* --yank：往复拽光标（验证输入取证用） */
    g_startTick = GetTickCount64();
    SetTimer(g_main, 11, 1000, nullptr);                   /* 兜底看门狗：超时/父进程消失即退出（1 秒轮询） */

    /* 一次性：4 秒后加载 --load 指定的 DLL（模拟"钩子模块晚于注入出现"） */
    if (cmdLine != nullptr)
    {
        /* --fight：200ms 抢一次全屏（横跳场景） */
        if (wcsstr(cmdLine, L"--fight") != nullptr)
            SetTimer(g_main, 7, 200, nullptr);

        const wchar_t* loadArg = wcsstr(cmdLine, L"--load ");
        if (loadArg != nullptr)
        {
            std::wstring p = yz::Trim(std::wstring(loadArg + 7));
            if (!p.empty() && p[0] == L'"')
            {
                const size_t e = p.find(L'"', 1);
                if (e != std::wstring::npos)
                    p = p.substr(1, e - 1);
            }
            else
            {
                const size_t sp = p.find(L' ');
                if (sp != std::wstring::npos)
                    p = p.substr(0, sp);
            }
            g_lateLoadDll = p;
            if (!g_lateLoadDll.empty())
                SetTimer(g_main, 6, 4000, nullptr);
        }
    }

    g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHookProc, nullptr, 0);
    ClipCursor(nullptr);

    BOOL hotkeyOk = RegisterHotKey(g_main, 1, MOD_CONTROL | MOD_ALT, VK_F9);
    AppendFile(yz::JoinPath(g_exeDir, L"sim_log.txt"),
               yz::Format(L"start tick=%u keyHook=%p registerHotKey(F9)=%d",
                          GetTickCount(), g_keyHook, hotkeyOk ? 1 : 0));

    /* --hook / --hook-thread：模拟"客户端自己装钩子、并且把 SetWindowsHookEx 的返回值当生死判据"。
       现场证据（2026-10-09）：v0.6.10 把安装拦成 NULL 时 Yistart.exe 8 毫秒内退出，守卫 4 秒后
       拉起，形成十秒级重启循环（电子点名窗口因此立不住）。所以回归必须**复现这个致命反应**——
       否则模拟目标对 NULL 无动于衷，"空过程替换"这条修复就没有防线。
         --hook        hMod = 本 exe 模块（与现场"厂商 DLL 传自己模块"同构，命中模块路径判据）
         --hook-thread hMod = NULL + 本线程 tid（命中"回调地址反查模块"那条判据）
       两者都要求模拟目标被放在**路径含 GZYZ** 的目录里，否则 ShouldBlockHook 会正确地放行。
       **延迟 8 秒**再装：现场那次是"注入之后"才装钩子的，启动时立刻装会走系统原生路径、
       完全绕开我们的钩子（第一版就是这么写的，实测两边都不命中）。 */
    if (cmdLine != nullptr)
    {
        if ((wcsstr(cmdLine, L"--hook") != nullptr))
        {
            g_hookThreadMode  = (wcsstr(cmdLine, L"--hook-thread") != nullptr);
            g_hookMode        = !g_hookThreadMode;
            g_hookMessageMode = (wcsstr(cmdLine, L"--hook-message") != nullptr);
            SetTimer(g_main, 8, 8000, nullptr);
        }
    }

    /* 本节只是把 --hook 的开关记下来并挂一个"8 秒后执行"的定时器；
       真正的安装与"拿 NULL 就自杀"在 InstallSelfHookOrDie() 里（见 WM_TIMER 分支）。
       为什么必须延迟：现场那次客户端是在 Trainer 已经注入之后才装钩子的；
       如果在启动时（= 注入之前）就装，走的是系统原生路径、根本不会经过我们的钩子。 */

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
    if (g_msgHook != nullptr)
        UnhookWindowsHookEx(g_msgHook);
    ClipCursor(nullptr);
    return g_exitCode;
}


