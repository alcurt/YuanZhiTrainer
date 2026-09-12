#include "yz_log.h"
#include "yz_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <vector>

namespace
{
const unsigned long long kMaxLogSize = 2ull * 1024ull * 1024ull;

HANDLE           g_file      = INVALID_HANDLE_VALUE;
INIT_ONCE        g_once      = INIT_ONCE_STATIC_INIT;
CRITICAL_SECTION g_cs;
std::wstring     g_tag;
int              g_level     = yz::kLogInfo;
yz::LogSink      g_sink      = nullptr;
void*            g_sinkCtx   = nullptr;
int              g_fileIndex = 0;

BOOL CALLBACK InitOnceProc(PINIT_ONCE, PVOID, PVOID*)
{
    InitializeCriticalSection(&g_cs);
    return TRUE;
}

void EnsureInit()
{
    InitOnceExecuteOnce(&g_once, InitOnceProc, nullptr, nullptr);
}

const wchar_t* LevelName(int level)
{
    switch (level)
    {
    case yz::kLogError: return L"ERR";
    case yz::kLogWarn:  return L"WRN";
    case yz::kLogInfo:  return L"INF";
    default:            return L"DBG";
    }
}

void CloseFile()
{
    if (g_file != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void OpenFile()
{
    std::wstring path = yz::LogFilePath();
    if (path.empty())
        return;

    /* 超限则顺延到下一个编号，不删除任何旧日志 */
    for (;;)
    {
        HANDLE probe = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        unsigned long long size = 0;
        if (probe != INVALID_HANDLE_VALUE)
        {
            LARGE_INTEGER li;
            li.QuadPart = 0;
            if (GetFileSizeEx(probe, &li))
                size = static_cast<unsigned long long>(li.QuadPart);
            CloseHandle(probe);
        }
        if (size < kMaxLogSize)
            break;

        g_fileIndex++;
        wchar_t name[64];
        _snwprintf_s(name, _TRUNCATE, L"yzt-%d.log", g_fileIndex);
        path = yz::JoinPath(yz::LogDir(), name);
    }

    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}
} /* namespace */

namespace yz
{
void LogInit(const wchar_t* tag)
{
    EnsureInit();
    EnterCriticalSection(&g_cs);
    g_tag = (tag != nullptr) ? tag : L"YZ";
    LeaveCriticalSection(&g_cs);
}

void LogShutdown()
{
    EnsureInit();
    EnterCriticalSection(&g_cs);
    CloseFile();
    LeaveCriticalSection(&g_cs);
}

void LogSetLevel(int level)
{
    EnsureInit();
    g_level = level;
}

int LogLevel()
{
    return g_level;
}

void LogSetSink(LogSink sink, void* ctx)
{
    EnsureInit();
    EnterCriticalSection(&g_cs);
    g_sink    = sink;
    g_sinkCtx = ctx;
    LeaveCriticalSection(&g_cs);
}

std::wstring LogDir()
{
    return JoinPath(GetTempDir(), L"YZTrainer");
}

std::wstring LogFilePath()
{
    return JoinPath(LogDir(), L"yzt.log");
}

void LogWrite(int level, const wchar_t* fmt, ...)
{
    EnsureInit();
    if (level > g_level)
        return;

    std::wstring text;
    va_list args;
    va_start(args, fmt);
    {
        int need = _vscwprintf(fmt, args);
        if (need > 0)
        {
            std::vector<wchar_t> buf(static_cast<size_t>(need) + 1, L'\0');
            va_start(args, fmt);
            _vsnwprintf_s(buf.data(), buf.size(), _TRUNCATE, fmt, args);
            va_end(args);
            text.assign(buf.data());
        }
    }
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);

    std::wstring tagCopy;
    EnterCriticalSection(&g_cs);
    tagCopy = g_tag.empty() ? L"YZ" : g_tag;
    LeaveCriticalSection(&g_cs);

    std::wstring line = Format(L"[%04d-%02d-%02d %02d:%02d:%02d.%03d][T%04u][%s][%s] %s",
                               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                               st.wMilliseconds, GetCurrentThreadId(),
                               tagCopy.c_str(), LevelName(level), text.c_str());

    LogSink sink = nullptr;
    void*   ctx  = nullptr;

    EnterCriticalSection(&g_cs);
    sink = g_sink;
    ctx  = g_sinkCtx;
    LeaveCriticalSection(&g_cs);

    EnterCriticalSection(&g_cs);
    EnsureDirectory(LogDir());
    if (g_file == INVALID_HANDLE_VALUE)
        OpenFile();
    if (g_file != INVALID_HANDLE_VALUE)
    {
        std::string utf8 = WideToUtf8(line + L"\r\n");
        DWORD written = 0;
        WriteFile(g_file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    LeaveCriticalSection(&g_cs);

    OutputDebugStringW((line + L"\n").c_str());
    if (sink != nullptr)
        sink(level, line.c_str(), ctx);
}
} /* namespace yz */
