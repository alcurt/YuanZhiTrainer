#include "app.h"

#include "yz_log.h"
#include "yz_util.h"

#include <vector>

namespace
{
HANDLE        g_stopEvent = nullptr;
HANDLE        g_ioEvent   = nullptr;
HANDLE        g_thread    = nullptr;
volatile LONG g_stop      = 0;

void PostStatusToUi()
{
    if (g_app.hwndMain != nullptr)
        PostMessageW(g_app.hwndMain, WM_YZ_STATUS, 0, 0);
}

bool ReadExactOverlapped(HANDLE pipe, void* buf, DWORD len)
{
    BYTE* p = static_cast<BYTE*>(buf);
    DWORD done = 0;
    while (done < len)
    {
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = g_ioEvent;
        DWORD got = 0;
        BOOL ok = ReadFile(pipe, p + done, len - done, &got, &ov);
        if (!ok)
        {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING)
                return false;
            HANDLE waits[2] = { g_stopEvent, g_ioEvent };
            DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0)
            {
                CancelIo(pipe);
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

bool ReadFrame(HANDLE pipe, DWORD* opcode, std::vector<BYTE>* payload)
{
    YZ_FRAME_HEADER hdr;
    if (!ReadExactOverlapped(pipe, &hdr, sizeof(hdr)))
        return false;
    if (hdr.magic != YZ_FRAME_MAGIC || hdr.length > YZ_MAX_FRAME_PAYLOAD)
        return false;
    payload->resize(hdr.length);
    if (hdr.length != 0 && !ReadExactOverlapped(pipe, payload->data(), hdr.length))
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

void ServerThread()
{
    YZLOGI(L"管道服务线程启动");

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
                    CancelIo(pipe);
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

        EnterCriticalSection(&g_app.cs);
        g_app.pipe            = pipe;
        g_app.clientPid       = clientPid;
        g_app.clientConnected = true;
        LeaveCriticalSection(&g_app.cs);

        YZLOGI(L"Hook DLL 已连接: pid=%u", clientPid);
        UiAppendLog(yz::kLogInfo, yz::Format(L"Hook DLL 已连接 (PID=%u)", clientPid));
        PostStatusToUi();
        IpcSendConfig();

        for (;;)
        {
            if (InterlockedCompareExchange(&g_stop, 0, 0) != 0)
                break;
            DWORD opcode = 0;
            std::vector<BYTE> payload;
            if (!ReadFrame(pipe, &opcode, &payload))
                break;
            HandleEvent(opcode, payload);
        }

        EnterCriticalSection(&g_app.cs);
        if (g_app.pipe == pipe)
            g_app.pipe = INVALID_HANDLE_VALUE;
        g_app.clientConnected = false;
        g_app.clientPid       = 0;
        LeaveCriticalSection(&g_app.cs);

        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);

        YZLOGI(L"Hook DLL 连接结束: pid=%u", clientPid);
        UiAppendLog(yz::kLogWarn, yz::Format(L"Hook DLL 连接结束 (PID=%u)", clientPid));
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

bool IpcSend(DWORD opcode, const void* payload, DWORD len)
{
    bool ok = false;
    EnterCriticalSection(&g_app.cs);
    if (g_app.pipe != INVALID_HANDLE_VALUE && g_app.pipe != nullptr)
        ok = yz::SendFrame(g_app.pipe, opcode, payload, len);
    LeaveCriticalSection(&g_app.cs);
    return ok;
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
