#include "engine.h"

#include "exam.h"
#include "hookmgr.h"
#include "hooks_capture.h"
#include "hooks_input.h"
#include "hooks_window.h"
#include "native_unhook.h"
#include "policy.h"
#include "yz_hook_state.h"

#include <deque>
#include <string.h>
#include <string>
#include <vector>

namespace yzhook
{
volatile LONG g_flags           = 0;
volatile LONG g_examMode        = 0;
DWORD         g_windowPercent   = 60;
volatile LONG g_windowizeCount  = 0;
volatile LONG g_hooksInstalled  = 0;
DWORD         g_lastError       = 0;
std::wstring  g_targetDir;
} /* namespace yzhook */

namespace
{
__declspec(thread) int t_depth = 0;

CRITICAL_SECTION g_cs;
bool             g_csInit = false;

HANDLE           g_stopEvent = nullptr;
HANDLE           g_wakeEvent = nullptr;
HANDLE           g_thread    = nullptr;
HANDLE           g_pipe      = INVALID_HANDLE_VALUE;

std::deque<YZ_LOG_EVENT> g_logQueue;
std::deque<DWORD>        g_statusQueue;

DWORD            g_configFlags = 0;
bool             g_running = false;
bool             g_unloadRequested = false;
bool             g_hooksReady = false;

void Lock()   { if (g_csInit) EnterCriticalSection(&g_cs); }
void Unlock() { if (g_csInit) LeaveCriticalSection(&g_cs); }

void DisconnectPipe()
{
    if (g_pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
        YZLOGW(L"与主程序的管道连接已断开");
    }
}

void FillStatus(YZ_STATUS* status, DWORD opcodeUnused)
{
    (void)opcodeUnused;
    ZeroMemory(status, sizeof(*status));
    status->size           = sizeof(YZ_STATUS);
    status->version        = YZ_PROTOCOL_VERSION;
    status->pid            = GetCurrentProcessId();
    status->flags          = static_cast<DWORD>(yzhook::g_flags);
    status->hooksInstalled = yzhook::HookActiveCount();
    status->windowizeCount = static_cast<DWORD>(yzhook::g_windowizeCount);
    status->examMode       = static_cast<DWORD>(yzhook::g_examMode);
    status->lastError      = yzhook::g_lastError;
    std::wstring host = yz::GetSelfPath();
    wcsncpy_s(status->hostPath, MAX_PATH, host.c_str(), _TRUNCATE);
}

void ApplyEffectiveFlags()
{
    DWORD effective = g_configFlags;
    if (yzhook::g_examMode != 0)
        effective = 0;
    InterlockedExchange(&yzhook::g_flags, static_cast<LONG>(effective));

    bool wantFrozen = (effective & YZ_FLAG_ANTI_MONITOR) != 0;
    if (wantFrozen && !yzhook::CaptureIsFrozen())
        yzhook::CaptureFreeze();
    else if (!wantFrozen && yzhook::CaptureIsFrozen())
        yzhook::CaptureUnfreeze();

    /* 解锁功能生效时，主动调用远志自己导出的卸载入口，拔掉"注入之前"就已经装好的
       全局钩子（用户态没有受支持的 API 能摘别人的钩子，但远志自己给了入口）。
       考试模式下 effective 已被清零，这里自然不会触发；函数内部一次性执行。 */
    if ((effective & YZ_FLAG_INPUT_UNLOCK) != 0)
        yzhook::NativeUnhookClientHooks(1500);
}

void ExamTick()
{
    const wchar_t* reason = nullptr;
    bool exam = yzhook::ExamDetect(&reason);
    LONG before = yzhook::g_examMode;

    if (exam && before == 0)
    {
        InterlockedExchange(&yzhook::g_examMode, 1);
        yzhook::PolicyRestore();
        ApplyEffectiveFlags();
        yzhook::SendLogToHost(YZ_LOG_WARN,
            yz::Format(L"检测到考试/测验模式(%s)，已停用全部功能", reason != nullptr ? reason : L"未知").c_str());
        yzhook::SendStatusToHost(YZ_EVT_EXAM_MODE);
    }
    else if (!exam && before != 0)
    {
        InterlockedExchange(&yzhook::g_examMode, 0);
        ApplyEffectiveFlags();
        yzhook::SendLogToHost(YZ_LOG_INFO, L"考试模式结束，功能按当前配置恢复");
        yzhook::SendStatusToHost(YZ_EVT_EXAM_MODE);
    }
}

void SweepTick()
{
    if (yzhook::g_examMode != 0)
        return;
    yzhook::WindowSweepTick();
    yzhook::InputEnforceTick();
    yzhook::PolicyEnforce();
    InterlockedExchange(&yzhook::g_hooksInstalled, static_cast<LONG>(yzhook::HookActiveCount()));
}

void FlushOutgoing()
{
    if (g_pipe == INVALID_HANDLE_VALUE)
        return;

    std::deque<YZ_LOG_EVENT> logs;
    std::deque<DWORD>        statuses;
    Lock();
    logs.swap(g_logQueue);
    statuses.swap(g_statusQueue);
    Unlock();

    for (size_t i = 0; i < logs.size(); i++)
    {
        if (!yz::SendFrame(g_pipe, YZ_EVT_LOG, &logs[i], sizeof(YZ_LOG_EVENT)))
        {
            DisconnectPipe();
            return;
        }
    }

    for (size_t i = 0; i < statuses.size(); i++)
    {
        YZ_STATUS st;
        FillStatus(&st, statuses[i]);
        if (!yz::SendFrame(g_pipe, statuses[i], &st, sizeof(st)))
        {
            DisconnectPipe();
            return;
        }
    }
}

void HandleCommand(DWORD opcode, const std::vector<BYTE>& payload)
{
    DWORD value = 0;
    bool hasValue = payload.size() >= sizeof(DWORD);
    if (hasValue)
        memcpy(&value, payload.data(), sizeof(DWORD));

    switch (opcode)
    {
    case YZ_CMD_APPLY_CONFIG:
        if (payload.size() >= sizeof(YZ_CONFIG))
        {
            YZ_CONFIG cfg;
            memcpy(&cfg, payload.data(), sizeof(cfg));
            yzhook::EngineApplyConfig(cfg);
        }
        break;

    case YZ_CMD_SET_WINDOW_MODE:
        yzhook::EngineSetFlag(YZ_FLAG_WINDOWIZE, hasValue ? (value != 0) : true);
        break;

    case YZ_CMD_SET_INPUT_UNLOCK:
        yzhook::EngineSetFlag(YZ_FLAG_INPUT_UNLOCK, hasValue ? (value != 0) : true);
        break;

    case YZ_CMD_SET_ANTI_MONITOR:
        yzhook::EngineSetFlag(YZ_FLAG_ANTI_MONITOR, hasValue ? (value != 0) : true);
        break;

    case YZ_CMD_SET_BLOCK_REMOTE:
        yzhook::EngineSetFlag(YZ_FLAG_BLOCK_REMOTE, hasValue ? (value != 0) : true);
        break;

    case YZ_CMD_QUERY_STATUS:
        yzhook::SendStatusToHost(YZ_EVT_STATUS);
        break;

    case YZ_CMD_UNLOAD:
        YZLOGI(L"收到卸载指令");
        g_unloadRequested = true;
        break;

    default:
        YZLOGD(L"收到未知指令: %u", opcode);
        break;
    }
}

void IpcTick()
{
    if (g_pipe == INVALID_HANDLE_VALUE)
    {
        HANDLE h = CreateFileW(YZ_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            g_pipe = h;
            YZLOGI(L"已连接到主程序管道");
            YZ_STATUS st;
            FillStatus(&st, 0);
            yz::SendFrame(g_pipe, YZ_EVT_HELLO, &st, sizeof(st));
        }
        return;
    }

    FlushOutgoing();
    if (g_pipe == INVALID_HANDLE_VALUE)
        return;

    DWORD available = 0;
    if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &available, nullptr))
    {
        DisconnectPipe();
        return;
    }

    while (available >= sizeof(YZ_FRAME_HEADER))
    {
        DWORD opcode = 0;
        std::vector<BYTE> payload;
        if (!yz::RecvFrame(g_pipe, &opcode, &payload))
        {
            DisconnectPipe();
            return;
        }
        HandleCommand(opcode, payload);

        available = 0;
        if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &available, nullptr))
        {
            DisconnectPipe();
            return;
        }
    }
}

void EngineThreadProc()
{
    YZLOGI(L"YZHook 引擎线程启动");

    if (!yzhook::EngineIsTargetHost())
    {
        YZLOGI(L"宿主进程不是远志学生端/模拟目标，Hook 不启用");
        return;
    }

    if (!yzhook::HookInit())
    {
        YZLOGE(L"MinHook 初始化失败，Hook 未启用");
        return;
    }

    yzhook::WindowHooksInstall(true);
    yzhook::InputHooksInstall(true);
    yzhook::CaptureHooksInstall(true);
    yzhook::PolicyBackup();
    g_hooksReady = true;

    InterlockedExchange(&yzhook::g_hooksInstalled, static_cast<LONG>(yzhook::HookActiveCount()));
    YZLOGI(L"Hook 安装完成，共 %u 个", static_cast<unsigned>(yzhook::HookActiveCount()));
    yzhook::SendLogToHost(YZ_LOG_INFO, L"已注入远志学生端，等待主程序配置");

    DWORD lastSweep = 0;
    DWORD lastExam  = 0;

    while (!g_unloadRequested)
    {
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0)
            break;

        IpcTick();

        DWORD now = GetTickCount();
        if (now - lastSweep >= 500)
        {
            lastSweep = now;
            SweepTick();
        }
        if (now - lastExam >= 2000)
        {
            lastExam = now;
            ExamTick();
        }

        WaitForSingleObject(g_wakeEvent, 100);
    }

    YZLOGI(L"引擎线程退出，开始还原");
    yzhook::CaptureUnfreeze();
    yzhook::PolicyRestore();
    if (g_hooksReady)
    {
        yzhook::HookDetachAll();
        yzhook::HookUninit();
        g_hooksReady = false;
    }

    if (g_pipe != INVALID_HANDLE_VALUE)
    {
        YZ_STATUS st;
        FillStatus(&st, 0);
        yz::SendFrame(g_pipe, YZ_EVT_STATUS, &st, sizeof(st));
        DisconnectPipe();
    }
    g_running = false;
}

DWORD WINAPI EngineThreadEntry(LPVOID)
{
    EngineThreadProc();
    return 0;
}
} /* namespace */

namespace yzhook
{
bool TryEnterHook()
{
    if (t_depth > 0)
        return false;
    t_depth++;
    return true;
}

void LeaveHook()
{
    if (t_depth > 0)
        t_depth--;
}

void SendLogToHost(int level, const wchar_t* text)
{
    if (!g_csInit || text == nullptr)
        return;

    YZ_LOG_EVENT ev;
    ZeroMemory(&ev, sizeof(ev));
    ev.size  = sizeof(ev);
    ev.level = static_cast<DWORD>(level);
    ev.pid   = GetCurrentProcessId();
    wcsncpy_s(ev.text, YZ_MAX_LOG_TEXT, text, _TRUNCATE);

    Lock();
    if (g_logQueue.size() > 256)
        g_logQueue.pop_front();
    g_logQueue.push_back(ev);
    Unlock();

    if (g_wakeEvent != nullptr)
        SetEvent(g_wakeEvent);
}

void SendStatusToHost(DWORD opcode)
{
    if (!g_csInit)
        return;
    Lock();
    g_statusQueue.push_back(opcode);
    if (g_statusQueue.size() > 16)
        g_statusQueue.pop_front();
    Unlock();

    if (g_wakeEvent != nullptr)
        SetEvent(g_wakeEvent);
}

bool IsModulePathUnderTargetDir(HMODULE mod)
{
    std::wstring path = yz::GetModuleFilePath(mod);
    if (path.empty())
        return false;
    if (!g_targetDir.empty() && yz::IsUnderDir(path, g_targetDir))
        return true;
    return yz::ContainsNoCase(path, L"YZinfo Multimedia teaching software");
}

bool IsWindowOfThisProcess(HWND hwnd)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

void LogHooked(const wchar_t* fmt, ...)
{
    (void)fmt;
}

bool EngineIsTargetHost()
{
    std::wstring exe = yz::GetSelfPath();
    if (exe.empty())
        return false;
    if (yz::ContainsNoCase(exe, L"YZinfo Multimedia teaching software"))
        return true;
    if (yz::ContainsNoCase(exe, L"YZSimTarget.exe"))
        return true;
    if (GetModuleHandleW(L"Rmdesk.ads") != nullptr)
        return true;
    if (GetModuleHandleW(L"PlayerGUI.dll") != nullptr)
        return true;
    return false;
}

void EngineApplyConfig(const YZ_CONFIG& cfg)
{
    g_configFlags = cfg.flags;
    if (cfg.windowPercent >= 20 && cfg.windowPercent <= 100)
        g_windowPercent = cfg.windowPercent;
    WindowHooksSetTopmost((cfg.flags & YZ_FLAG_TOPMOST) != 0);
    ApplyEffectiveFlags();
    SendStatusToHost(YZ_EVT_STATUS);
    YZLOGI(L"应用配置: flags=0x%08X percent=%u", cfg.flags, g_windowPercent);
}

void EngineSetFlag(DWORD flag, bool on)
{
    if (on)
        g_configFlags |= flag;
    else
        g_configFlags &= ~flag;
    ApplyEffectiveFlags();
    SendStatusToHost(YZ_EVT_STATUS);
}

void EngineFillStatus(YZ_STATUS* status)
{
    if (status == nullptr)
        return;
    FillStatus(status, 0);
}

bool EngineIsExamMode()
{
    return g_examMode != 0;
}

void EngineStart()
{
    if (g_running)
        return;

    if (!g_csInit)
    {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }

    g_targetDir = yz::ToLower(yz::DirNameOf(yz::GetSelfPath()));
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (g_stopEvent == nullptr || g_wakeEvent == nullptr)
    {
        YZLOGE(L"EngineStart: 事件对象创建失败");
        return;
    }

    g_running = true;
    g_thread = CreateThread(nullptr, 0, EngineThreadEntry, nullptr, 0, nullptr);
    if (g_thread == nullptr)
    {
        YZLOGE(L"EngineStart: 工作线程创建失败");
        g_running = false;
    }
}

void EngineStop()
{
    if (g_stopEvent != nullptr)
        SetEvent(g_stopEvent);
    if (g_thread != nullptr)
    {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stopEvent != nullptr)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_wakeEvent != nullptr)
    {
        CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
    }
    g_running = false;
}
} /* namespace yzhook */

