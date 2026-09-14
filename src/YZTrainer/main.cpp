//
// YZTrainer.exe 主程序入口。
//
#include "app.h"

#include "payload.h"

#include "yz_log.h"
#include "yz_util.h"

AppRuntime g_app;

namespace
{
HANDLE        g_watchdogStop   = nullptr;
HANDLE        g_watchdogThread = nullptr;
volatile LONG g_stop           = 0;

void LogSinkProc(int level, const wchar_t* text, void*)
{
    if (text != nullptr)
        UiAppendLog(level, std::wstring(text));
}

DWORD WINAPI WatchdogEntry(LPVOID)
{
    YZLOGI(L"看门狗线程启动");
    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
    {
        WatchdogTick();
        WaitForSingleObject(g_watchdogStop, 1000);
    }
    YZLOGI(L"看门狗线程退出");
    return 0;
}

std::wstring StartupSummary()
{
    bool elevated = false;
    yz::IsProcessElevated(&elevated);
    std::wstring hookPath = ResolveHookDllPath();

    std::wstring text = yz::Format(L"YZTrainer %s 启动。管理权限: %s\r\n",
                                   YZ_VERSION_STR, elevated ? L"是" : L"否（注入可能失败）");
    text += yz::Format(L"Hook DLL: %s [来源=%s 内嵌资源=%u 字节] (%s)\r\n", hookPath.c_str(),
                       g_app.hookDllSource.c_str(), PayloadEmbeddedSize(),
                       yz::FileExists(hookPath) ? L"存在" : L"缺失，请先编译 YZHook 工程");
    if (!g_app.hookDllError.empty())
        text += yz::Format(L"Hook DLL 释放失败原因: %s\r\n", g_app.hookDllError.c_str());
    text += yz::Format(L"配置: 窗口化=%d 解锁=%d 置顶=%d 防监视=%d 拦遥控=%d 宽度=%u%% 考试守护=%d\r\n",
                       (g_app.cfg.flags & YZ_FLAG_WINDOWIZE) ? 1 : 0,
                       (g_app.cfg.flags & YZ_FLAG_INPUT_UNLOCK) ? 1 : 0,
                       (g_app.cfg.flags & YZ_FLAG_TOPMOST) ? 1 : 0,
                       (g_app.cfg.flags & YZ_FLAG_ANTI_MONITOR) ? 1 : 0,
                       (g_app.cfg.flags & YZ_FLAG_BLOCK_REMOTE) ? 1 : 0,
                       g_app.cfg.windowPercent,
                       g_app.cfg.enableExamGuard ? 1 : 0);
    text += L"提示：本工具不修改远志文件、不卸载驱动；检测到考试强信号时会自动停用全部功能。";
    return text;
}
} /* namespace */

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int)
{
    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE, L"Global\\YZTrainer_SingleInstance");
    if (instanceMutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        MessageBoxW(nullptr, L"YZTrainer 已经在运行。", L"YZTrainer", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    InitializeCriticalSection(&g_app.cs);
    g_app.pipe    = INVALID_HANDLE_VALUE;
    g_app.exeDir  = yz::GetExeDir();
    g_app.iniPath = ConfigPath();

    yz::LogInit(L"YZTrainer");
    ConfigLoad(g_app.cfg, g_app.iniPath);
    yz::LogSetLevel(g_app.cfg.logLevel);
    yz::LogSetSink(LogSinkProc, nullptr);
    YZLOGI(L"YZTrainer 启动，exeDir=%s", g_app.exeDir.c_str());
    if (!PayloadCheckEmbedded())
        YZLOGW(L"内嵌 Hook 自检未通过：资源缺失或架构与本进程不一致，将依赖外部 YZHook.dll");

    IpcStart();

    HWND hwnd = UiInit(hinst);
    if (hwnd == nullptr)
    {
        YZLOGE(L"主窗口创建失败，退出");
        IpcStop();
        yz::LogShutdown();
        return 1;
    }

    UiAppendLog(yz::kLogInfo, StartupSummary());

    g_watchdogStop   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_watchdogThread = CreateThread(nullptr, 0, WatchdogEntry, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    YZLOGI(L"准备退出");
    InterlockedExchange(&g_stop, 1);
    if (g_watchdogStop != nullptr)
        SetEvent(g_watchdogStop);
    if (g_watchdogThread != nullptr)
    {
        WaitForSingleObject(g_watchdogThread, 3000);
        CloseHandle(g_watchdogThread);
        g_watchdogThread = nullptr;
    }
    if (g_watchdogStop != nullptr)
    {
        CloseHandle(g_watchdogStop);
        g_watchdogStop = nullptr;
    }

    IpcSendUnload();
    Sleep(300);
    IpcStop();
    UiShutdown();
    ConfigSave(g_app.cfg, g_app.iniPath);
    yz::LogShutdown();
    DeleteCriticalSection(&g_app.cs);

    if (instanceMutex != nullptr)
        CloseHandle(instanceMutex);
    return 0;
}
