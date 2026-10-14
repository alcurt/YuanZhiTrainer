#include "app.h"
#include "capturechild.h"

#include "yz_log.h"
#include "yz_util.h"

#include <vector>

/* 命名管道服务端。
 *
 * v0.6.38 起支持**多客户端**：原来同一时刻只服务一个连接（监听线程自己既接入又服务），
 * 于是"学生端先占住管道"之后，我们后注入的**抓屏子进程永远连不上**、拿不到配置帧
 * ——v0.6.33 的冻结因此不会在机房里被武装（v0.6.36 先用 capture-state.txt 绕过，
 * 那是补充通道，正式修法在这里）。
 *
 * 现在的分工：
 *   监听线程：只负责 CreateNamedPipe + ConnectNamedPipe，接到一个就交给独立服务线程，
 *             自己立刻回去接下一个（PIPE_UNLIMITED_INSTANCES 允许同名多实例并发）。
 *   服务线程：每个连接一个，读帧 → HandleEvent。
 *
 * 两个必须注意的点（多客户端特有的坑）：
 *   1) overlapped 事件**不能共用**：每个连接必须有自己的事件，否则多个线程的
 *      WaitForMultipleObjects 会被同一个事件唤醒、读到别人的结果。所以 g_ioEvent 只留给
 *      监听线程用，服务线程用各自 slot 的 ioEvent。
 *   2) 停止时要让阻塞在 ReadFile 上的服务线程醒来：必须用 CancelIoEx（CancelIo 只取消
 *      调用线程自己的操作），再等线程退出，避免关进程时挂住。
 */
namespace
{
HANDLE        g_stopEvent = nullptr;
HANDLE        g_ioEvent   = nullptr;   /* 只给监听线程用（见上方说明） */
HANDLE        g_thread    = nullptr;
volatile LONG g_stop      = 0;

const int kMaxClients = 8;

struct ClientSlot
{
    HANDLE pipe   = INVALID_HANDLE_VALUE;
    HANDLE ioEvent = nullptr;
    HANDLE thread = nullptr;
    DWORD  pid    = 0;
};

ClientSlot g_clients[kMaxClients];   /* 受 g_app.cs 保护 */

void PostStatusToUi()
{
    if (g_app.hwndMain != nullptr)
        PostMessageW(g_app.hwndMain, WM_YZ_STATUS, 0, 0);
}

/* 回收已经退出的服务线程句柄，挑一个空闲槽。调用者须持有 g_app.cs。
   返回槽下标，满则返回 -1。 */
int AllocSlotLocked()
{
    int freeIdx = -1;
    for (int i = 0; i < kMaxClients; i++)
    {
        ClientSlot& s = g_clients[i];
        if (s.thread != nullptr && s.pipe == INVALID_HANDLE_VALUE &&
            WaitForSingleObject(s.thread, 0) == WAIT_OBJECT_0)
        {
            CloseHandle(s.thread);
            s.thread = nullptr;
        }
        if (freeIdx < 0 && s.pipe == INVALID_HANDLE_VALUE && s.thread == nullptr)
            freeIdx = i;
    }
    return freeIdx;
}

/* 把"主客户端"（连接顺序里的第一个）同步到 g_app 的旧字段上：
   UI 状态显示、诊断包、注入判定都还在读这些字段，语义保持"第一个连上的那条"。 */
void SyncPrimaryLocked()
{
    bool    any  = false;
    DWORD   pid  = 0;
    HANDLE  pipe = INVALID_HANDLE_VALUE;
    for (int i = 0; i < kMaxClients; i++)
    {
        const ClientSlot& s = g_clients[i];
        if (s.pipe != INVALID_HANDLE_VALUE)
        {
            any  = true;
            pid  = s.pid;
            pipe = s.pipe;
            break;
        }
    }
    g_app.clientConnected = any;
    g_app.clientPid       = any ? pid : 0;
    g_app.pipe            = any ? pipe : INVALID_HANDLE_VALUE;
}

/* 某个 pid 是否已经有一个连上的客户端（注入判定用：连上了就别再重复注入）。 */
bool IsPidConnectedLocked(DWORD pid)
{
    for (int i = 0; i < kMaxClients; i++)
        if (g_clients[i].pipe != INVALID_HANDLE_VALUE && g_clients[i].pid == pid)
            return true;
    return false;
}

bool ReadExactOverlapped(HANDLE pipe, void* buf, DWORD len, HANDLE ioEvent)
{
    BYTE* p = static_cast<BYTE*>(buf);
    DWORD done = 0;
    while (done < len)
    {
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = ioEvent;
        DWORD got = 0;
        BOOL ok = ReadFile(pipe, p + done, len - done, &got, &ov);
        if (!ok)
        {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING)
                return false;
            HANDLE waits[2] = { g_stopEvent, ioEvent };
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0)
            {
                CancelIoEx(pipe, &ov);
                return false;
            }
            if (!GetOverlappedResult(pipe, &ov, &got, FALSE))
                return false;
        }
        if (got == 0)
            return false;
        done += got;
    }
    return true;
}

bool ReadFrame(HANDLE pipe, HANDLE ioEvent, DWORD* opcode, std::vector<BYTE>* payload)
{
    YZ_FRAME_HEADER hdr;
    if (!ReadExactOverlapped(pipe, &hdr, sizeof(hdr), ioEvent))
        return false;
    if (hdr.magic != YZ_FRAME_MAGIC || hdr.length > YZ_MAX_FRAME_PAYLOAD)
        return false;
    payload->resize(hdr.length);
    if (hdr.length != 0 && !ReadExactOverlapped(pipe, payload->data(), hdr.length, ioEvent))
        return false;
    if (opcode != nullptr)
        *opcode = hdr.opcode;
    return true;
}

void HandleEvent(DWORD opcode, const std::vector<BYTE>& payload)
{
    switch (opcode)
    {
    case YZ_EVT_LOG:
        if (payload.size() >= sizeof(YZ_LOG_EVENT))
        {
            const YZ_LOG_EVENT* ev = reinterpret_cast<const YZ_LOG_EVENT*>(payload.data());
            std::wstring text(ev->text);
            YZLOGI(L"[Hook pid=%u] %s", ev->pid, text.c_str());

            /* 抓屏子进程（v0.6.32）：钩子侧在审计行里打了 [抓屏子进程 pid=N] 标记，
               这里认出来交给注入器按 PID 注入——冻结画面要在那个进程里做。
               解析抽成了纯函数（capturechild.h），好在 tests\unit 里直接跑单元测试。
               走日志通道而不是新开协议事件，是为了避免从任意线程直接往管道发帧。 */
            {
                DWORD childPid = 0;
                if (yzcap::ParseCaptureChildMarker(text, &childPid))
                    OnCaptureChildSeen(childPid, text);
            }

            /* 客户端 Hook 上报"广播窗口已窗口化/已假全屏"= 广播确实开始了：
               立刻静默导出一份诊断包（每进程一次）。广播常常没预告、一节课只有一次，
               靠人守着导出必然漏（2026-10-08 实测）。 */
            if (text.find(L"广播窗口已") != std::wstring::npos)
                AutoExportOnBroadcastOnce(L"首次识别到广播窗口（客户端 Hook 上报）");

            if (g_app.hwndMain != nullptr)
            {
                std::wstring* copy = new std::wstring(text);
                PostMessageW(g_app.hwndMain, WM_YZ_LOG, static_cast<WPARAM>(ev->level),
                             reinterpret_cast<LPARAM>(copy));
            }
        }
        break;

    case YZ_EVT_HELLO:
    case YZ_EVT_STATUS:
    case YZ_EVT_EXAM_MODE:
        if (payload.size() >= sizeof(YZ_STATUS))
        {
            EnterCriticalSection(&g_app.cs);
            memcpy(&g_app.status, payload.data(), sizeof(YZ_STATUS));
            bool exam = g_app.status.examMode != 0;
            g_app.examMode = exam;
            LeaveCriticalSection(&g_app.cs);

            if (opcode == YZ_EVT_EXAM_MODE)
                UiNotifyExam(exam);
            PostStatusToUi();
        }
        break;

    default:
        break;
    }
}

/* 每个连接一个：读帧循环 + 收尾。param 是槽下标（不是指针，避免生命周期问题）。 */
DWORD WINAPI ClientThreadEntry(LPVOID param)
{
    const int idx = static_cast<int>(reinterpret_cast<INT_PTR>(param));

    HANDLE pipe    = INVALID_HANDLE_VALUE;
    HANDLE ioEvent = nullptr;
    DWORD  pid     = 0;
    EnterCriticalSection(&g_app.cs);
    pipe    = g_clients[idx].pipe;
    ioEvent = g_clients[idx].ioEvent;
    pid     = g_clients[idx].pid;
    LeaveCriticalSection(&g_app.cs);

    YZLOGI(L"Hook DLL 已连接: pid=%u", pid);
    UiAppendLog(yz::kLogInfo, yz::Format(L"Hook DLL 已连接 (PID=%u)", pid));
    PostStatusToUi();
    IpcSendConfig();   /* 广播：新连上的与既有的客户端都拿到最新配置 */

    for (;;)
    {
        if (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
            break;
        DWORD opcode = 0;
        std::vector<BYTE> payload;
        if (!ReadFrame(pipe, ioEvent, &opcode, &payload))
            break;
        HandleEvent(opcode, payload);
    }

    EnterCriticalSection(&g_app.cs);
    g_clients[idx].pipe = INVALID_HANDLE_VALUE;
    g_clients[idx].pid  = 0;
    SyncPrimaryLocked();
    LeaveCriticalSection(&g_app.cs);

    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    if (ioEvent != nullptr)
    {
        CloseHandle(ioEvent);
        EnterCriticalSection(&g_app.cs);
        g_clients[idx].ioEvent = nullptr;
        LeaveCriticalSection(&g_app.cs);
    }

    YZLOGI(L"Hook DLL 连接结束: pid=%u", pid);
    UiAppendLog(yz::kLogWarn, yz::Format(L"Hook DLL 连接结束 (PID=%u)", pid));
    PostStatusToUi();
    return 0;
}

void ServerThread()
{
    YZLOGI(L"管道服务线程启动（多客户端）");

    while (InterlockedCompareExchange(&g_stop, 0, 0) == 0)
    {
        HANDLE pipe = CreateNamedPipeW(YZ_PIPE_NAME,
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                                       PIPE_UNLIMITED_INSTANCES, 16384, 16384, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE)
        {
            YZLOGE(L"CreateNamedPipe 失败: %s", yz::Win32ErrorMessage(GetLastError()).c_str());
            Sleep(1000);
            continue;
        }

        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = g_ioEvent;
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        DWORD err = GetLastError();
        if (!connected)
        {
            if (err == ERROR_IO_PENDING)
            {
                HANDLE waits[2] = { g_stopEvent, g_ioEvent };
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0)
                {
                    CancelIoEx(pipe, &ov);
                    CloseHandle(pipe);
                    break;
                }
                DWORD dummy = 0;
                if (!GetOverlappedResult(pipe, &ov, &dummy, FALSE))
                {
                    CloseHandle(pipe);
                    continue;
                }
            }
            else if (err != ERROR_PIPE_CONNECTED)
            {
                CloseHandle(pipe);
                continue;
            }
        }

        DWORD clientPid = 0;
        GetNamedPipeClientProcessId(pipe, &clientPid);

        int idx = -1;
        EnterCriticalSection(&g_app.cs);
        idx = AllocSlotLocked();
        if (idx >= 0)
        {
            g_clients[idx].pipe    = pipe;
            g_clients[idx].pid     = clientPid;
            g_clients[idx].ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            g_clients[idx].thread  = CreateThread(nullptr, 0, ClientThreadEntry,
                                                  reinterpret_cast<LPVOID>(static_cast<INT_PTR>(idx)),
                                                  0, nullptr);
            if (g_clients[idx].thread == nullptr)
            {
                /* 线程建不起来：这个连接只能放弃，但别把槽占住 */
                if (g_clients[idx].ioEvent != nullptr)
                    CloseHandle(g_clients[idx].ioEvent);
                g_clients[idx] = ClientSlot();
            }
            SyncPrimaryLocked();
        }
        LeaveCriticalSection(&g_app.cs);

        if (idx < 0)
        {
            /* 槽位满（>8 个并发客户端）：礼貌地断开这一个，继续接别人 */
            YZLOGW(L"管道服务线程: 并发客户端已满（%d），拒绝 pid=%u", kMaxClients, clientPid);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }

        PostStatusToUi();
    }

    YZLOGI(L"管道服务线程退出");
}

DWORD WINAPI ServerThreadEntry(LPVOID)
{
    ServerThread();
    return 0;
}
} /* namespace */

bool IpcStart()
{
    g_app.pipe            = INVALID_HANDLE_VALUE;
    g_app.clientPid       = 0;
    g_app.clientConnected = false;
    for (int i = 0; i < kMaxClients; i++)
        g_clients[i] = ClientSlot();

    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_ioEvent   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stopEvent == nullptr || g_ioEvent == nullptr)
    {
        YZLOGE(L"IpcStart: 事件创建失败");
        return false;
    }

    g_thread = CreateThread(nullptr, 0, ServerThreadEntry, nullptr, 0, nullptr);
    if (g_thread == nullptr)
    {
        YZLOGE(L"IpcStart: 线程创建失败");
        return false;
    }
    return true;
}

void IpcStop()
{
    InterlockedExchange(&g_stop, 1);
    if (g_stopEvent != nullptr)
        SetEvent(g_stopEvent);

    /* 让阻塞在 ReadFile 上的服务线程醒来：CancelIoEx 是"取消该句柄上的所有挂起 IO"，
       进程退出路径上不做这一步会等满超时。 */
    EnterCriticalSection(&g_app.cs);
    for (int i = 0; i < kMaxClients; i++)
        if (g_clients[i].pipe != INVALID_HANDLE_VALUE)
            CancelIoEx(g_clients[i].pipe, nullptr);
    LeaveCriticalSection(&g_app.cs);

    if (g_thread != nullptr)
    {
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    /* 收服务线程：关闭各自的事件句柄并等它们退出（有上限，不挂住退出流程） */
    for (int i = 0; i < kMaxClients; i++)
    {
        HANDLE th = nullptr;
        EnterCriticalSection(&g_app.cs);
        th = g_clients[i].thread;
        g_clients[i].thread = nullptr;
        if (g_clients[i].pipe != INVALID_HANDLE_VALUE)
        {
            CancelIoEx(g_clients[i].pipe, nullptr);
            CloseHandle(g_clients[i].pipe);
            g_clients[i].pipe = INVALID_HANDLE_VALUE;
        }
        LeaveCriticalSection(&g_app.cs);
        if (th != nullptr)
        {
            WaitForSingleObject(th, 2000);
            CloseHandle(th);
        }
    }

    if (g_stopEvent != nullptr)
    {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_ioEvent != nullptr)
    {
        CloseHandle(g_ioEvent);
        g_ioEvent = nullptr;
    }
}

bool IpcIsConnected(DWORD* pid)
{
    bool connected = false;
    EnterCriticalSection(&g_app.cs);
    connected = g_app.clientConnected;
    if (pid != nullptr)
        *pid = g_app.clientPid;
    LeaveCriticalSection(&g_app.cs);
    return connected;
}

bool IpcIsClientPidConnected(DWORD pid)
{
    bool found = false;
    EnterCriticalSection(&g_app.cs);
    found = IsPidConnectedLocked(pid);
    LeaveCriticalSection(&g_app.cs);
    return found;
}

bool IpcSend(DWORD opcode, const void* payload, DWORD len)
{
    /* 广播给所有连上的客户端（现场至少有两个：学生端 + 抓屏子进程）。
       慢客户端会占着锁一小会儿，但帧很小（配置/开关命令），可以接受。 */
    bool any = false;
    EnterCriticalSection(&g_app.cs);
    for (int i = 0; i < kMaxClients; i++)
    {
        if (g_clients[i].pipe != INVALID_HANDLE_VALUE)
        {
            if (yz::SendFrame(g_clients[i].pipe, opcode, payload, len))
                any = true;
        }
    }
    LeaveCriticalSection(&g_app.cs);
    return any;
}

bool IpcSendConfig()
{
    YZ_CONFIG cfg;
    ZeroMemory(&cfg, sizeof(cfg));
    cfg.size          = sizeof(cfg);
    cfg.version       = YZ_PROTOCOL_VERSION;
    cfg.flags         = g_app.cfg.flags;
    cfg.windowPercent = g_app.cfg.windowPercent;
    return IpcSend(YZ_CMD_APPLY_CONFIG, &cfg, sizeof(cfg));
}

bool IpcSendFlagCommand(DWORD opcode, bool on)
{
    DWORD value = on ? 1u : 0u;
    return IpcSend(opcode, &value, sizeof(value));
}

bool IpcSendUnload()
{
    return IpcSend(YZ_CMD_UNLOAD, nullptr, 0);
}
