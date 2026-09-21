//
// 教师端远程执行审计（只记录，不拦截）。
//
// 依据：教师端的"远程执行命令/关机/重启"最终都落在学生端进程里调用
//   CreateProcessA/W、WinExec、ShellExecuteExW、ExitWindowsEx
// （JiYuTrainer 就是在同样这几个点上弹确认框的）。
//
// 本项目的硬边界是不干扰教师端、不改变远志行为，所以这里只做三件事：
//   1. 先调用原函数，拿到真实结果，再看结果写审计；
//   2. 写 INFO 日志给主程序（UI 日志 + yzt.log）；
//   3. 追加一行到 %TEMP%\YZTrainer\remote-exec.log（超过 2MB 顺延到 remote-exec-1.log，
//      不删除旧文件），便于整份拷回分析。
//
// 考试模式下一律不记录：我们的原则是"检测到考试就全部停用"，观测也不例外。
//
#include "hooks_exec.h"

#include "hookmgr.h"
#include "yz_hook_state.h"

#include <shellapi.h>

#include <stdio.h>
#include <string>
#include <vector>

namespace
{
typedef BOOL (WINAPI *PFN_CreateProcessW)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                          LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                          LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
typedef BOOL (WINAPI *PFN_CreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
                                          LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                          LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
typedef UINT (WINAPI *PFN_WinExec)(LPCSTR, UINT);
typedef BOOL (WINAPI *PFN_ShellExecuteExW)(SHELLEXECUTEINFOW*);
typedef BOOL (WINAPI *PFN_ExitWindowsEx)(UINT, DWORD);

PFN_CreateProcessW   g_realCreateProcessW   = nullptr;
PFN_CreateProcessA   g_realCreateProcessA   = nullptr;
PFN_WinExec          g_realWinExec          = nullptr;
PFN_ShellExecuteExW  g_realShellExecuteExW  = nullptr;
PFN_ExitWindowsEx    g_realExitWindowsEx    = nullptr;

volatile LONG              g_auditCount = 0;
volatile LONG              g_auditFailed = 0;
bool                       g_hooksReady = false;
bool                       g_csInit     = false;
CRITICAL_SECTION           g_cs;

const unsigned long long kMaxAuditSize = 2ull * 1024ull * 1024ull;

void Lock()   { if (g_csInit) EnterCriticalSection(&g_cs); }
void Unlock() { if (g_csInit) LeaveCriticalSection(&g_cs); }

std::wstring FromAnsi(const char* s)
{
    if (s == nullptr || *s == '\0')
        return std::wstring();
    int need = MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (need <= 1)
        return std::wstring();
    std::vector<wchar_t> buf(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, -1, buf.data(), need);
    return std::wstring(buf.data());
}

/* 单行里的换行会破坏审计文件的可读性（命令行经常带换行） */
std::wstring OneLine(const std::wstring& s)
{
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++)
    {
        const wchar_t ch = s[i];
        out += (ch == L'\r' || ch == L'\n' || ch == L'\t') ? L' ' : ch;
    }
    return out;
}

void AppendAuditFile(const std::wstring& line)
{
    const std::wstring dir = yz::LogDir();
    if (!yz::EnsureDirectory(dir))
        return;

    std::wstring path = yz::JoinPath(dir, L"remote-exec.log");

    Lock();
    /* 超限就顺延编号，永不删除旧文件（与 yzt.log 的策略一致） */
    for (int index = 0; index < 64; index++)
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
        if (size < kMaxAuditSize)
            break;

        wchar_t name[64];
        _snwprintf_s(name, _TRUNCATE, L"remote-exec-%d.log", index + 1);
        path = yz::JoinPath(dir, name);
    }

    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (InterlockedIncrement(&g_auditFailed) <= 3)
            YZLOGW(L"远程执行审计写文件失败: err=%u path=%s", GetLastError(), path.c_str());
        Unlock();
        return;
    }

    std::string utf8 = yz::WideToUtf8(OneLine(line) + L"\r\n");
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
    Unlock();
}

std::wstring ExeFromCommandLine(const std::wstring& cmdLine)
{
    std::wstring s = yz::Trim(cmdLine);
    if (s.empty())
        return s;
    if (s[0] == L'"')
    {
        size_t end = s.find(L'"', 1);
        if (end != std::wstring::npos)
            return s.substr(1, end - 1);
    }
    size_t sp = s.find(L' ');
    return (sp == std::wstring::npos) ? s : s.substr(0, sp);
}

/* kind: CreateProcessW / CreateProcessA / WinExec / ShellExecuteExW / ExitWindowsEx */
void AuditEvent(const wchar_t* kind, const std::wstring& exe, const std::wstring& cmdLine,
                const std::wstring& resultText)
{
    /* 考试模式：全部停用，观测也不例外 */
    if (yzhook::g_examMode != 0)
        return;

    const std::wstring targetDir = yzhook::g_targetDir;
    const bool selfCall = !exe.empty() &&
                          ((!targetDir.empty() && yz::IsUnderDir(exe, targetDir)) ||
                           yz::IsYuanzhiInstallPath(exe));
    const wchar_t* origin = selfCall ? L"self" : L"remote";

    SYSTEMTIME st;
    GetLocalTime(&st);
    const std::wstring line = yz::Format(
        L"[%04d-%02d-%02d %02d:%02d:%02d.%03d] pid=%u tid=%u kind=%s origin=%s exe=\"%s\" cmd=\"%s\" %s",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
        GetCurrentProcessId(), GetCurrentThreadId(), kind, origin,
        exe.c_str(), cmdLine.c_str(), resultText.c_str());

    InterlockedIncrement(&g_auditCount);
    AppendAuditFile(line);

    /* 界面日志只报"疑似教师端下发"的，避免客户端自己拉辅助进程时刷屏 */
    if (!selfCall)
    {
        yzhook::SendLogToHost(YZ_LOG_WARN,
            yz::Format(L"[远程执行审计] %s exe=%s cmd=%s %s",
                       kind, exe.empty() ? L"(未知)" : exe.c_str(),
                       cmdLine.empty() ? L"(无)" : cmdLine.c_str(),
                       resultText.c_str()).c_str());
    }
}
} /* namespace */

BOOL WINAPI Hook_CreateProcessW(LPCWSTR appName, LPWSTR cmdLine, LPSECURITY_ATTRIBUTES procAttr,
                                LPSECURITY_ATTRIBUTES threadAttr, BOOL inherit, DWORD flags,
                                LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si, LPPROCESS_INFORMATION pi)
{
    if (!yzhook::TryEnterHook())
        return g_realCreateProcessW(appName, cmdLine, procAttr, threadAttr, inherit, flags, env, cwd, si, pi);

    const BOOL result = g_realCreateProcessW(appName, cmdLine, procAttr, threadAttr, inherit, flags,
                                             env, cwd, si, pi);
    std::wstring exe = (appName != nullptr) ? std::wstring(appName) : ExeFromCommandLine(cmdLine != nullptr ? cmdLine : L"");
    AuditEvent(L"CreateProcessW", exe, cmdLine != nullptr ? cmdLine : L"",
               result ? (pi != nullptr ? yz::Format(L"ok pid=%u", pi->dwProcessId) : L"ok")
                      : yz::Format(L"失败 err=%u", GetLastError()));

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_CreateProcessA(LPCSTR appName, LPSTR cmdLine, LPSECURITY_ATTRIBUTES procAttr,
                                LPSECURITY_ATTRIBUTES threadAttr, BOOL inherit, DWORD flags,
                                LPVOID env, LPCSTR cwd, LPSTARTUPINFOA si, LPPROCESS_INFORMATION pi)
{
    if (!yzhook::TryEnterHook())
        return g_realCreateProcessA(appName, cmdLine, procAttr, threadAttr, inherit, flags, env, cwd, si, pi);

    const BOOL result = g_realCreateProcessA(appName, cmdLine, procAttr, threadAttr, inherit, flags,
                                             env, cwd, si, pi);
    std::wstring exe = (appName != nullptr) ? FromAnsi(appName) : ExeFromCommandLine(FromAnsi(cmdLine));
    AuditEvent(L"CreateProcessA", exe, FromAnsi(cmdLine),
               result ? (pi != nullptr ? yz::Format(L"ok pid=%u", pi->dwProcessId) : L"ok")
                      : yz::Format(L"失败 err=%u", GetLastError()));

    yzhook::LeaveHook();
    return result;
}

UINT WINAPI Hook_WinExec(LPCSTR cmdLine, UINT cmdShow)
{
    if (!yzhook::TryEnterHook())
        return g_realWinExec(cmdLine, cmdShow);

    const UINT result = g_realWinExec(cmdLine, cmdShow);
    AuditEvent(L"WinExec", ExeFromCommandLine(FromAnsi(cmdLine)), FromAnsi(cmdLine),
               (result > 31) ? L"ok" : yz::Format(L"失败 code=%u", result));

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_ShellExecuteExW(SHELLEXECUTEINFOW* info)
{
    if (!yzhook::TryEnterHook())
        return g_realShellExecuteExW(info);

    const BOOL result = g_realShellExecuteExW(info);
    std::wstring exe;
    std::wstring cmd;
    if (info != nullptr)
    {
        exe = (info->lpFile != nullptr) ? info->lpFile : L"";
        if (info->lpParameters != nullptr)
            cmd = info->lpParameters;
        if (info->lpVerb != nullptr && *info->lpVerb != L'\0')
            cmd = yz::Format(L"verb=%s %s", info->lpVerb, cmd.c_str());
    }
    AuditEvent(L"ShellExecuteExW", exe, cmd, result ? L"ok" : yz::Format(L"失败 err=%u", GetLastError()));

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_ExitWindowsEx(UINT flags, DWORD reason)
{
    if (!yzhook::TryEnterHook())
        return g_realExitWindowsEx(flags, reason);

    const BOOL result = g_realExitWindowsEx(flags, reason);
    const wchar_t* action = L"其它";
    if ((flags & EWX_POWEROFF) == EWX_POWEROFF)      action = L"关机";
    else if ((flags & EWX_REBOOT) == EWX_REBOOT)     action = L"重启";
    else if ((flags & EWX_LOGOFF) == EWX_LOGOFF)     action = L"注销";
    AuditEvent(L"ExitWindowsEx", action, yz::Format(L"flags=0x%08X", flags),
               result ? L"ok" : yz::Format(L"失败 err=%u", GetLastError()));

    yzhook::LeaveHook();
    return result;
}

namespace yzhook
{
bool ExecHooksInstall(bool enableNow)
{
    if (!g_csInit)
    {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }
    if (g_hooksReady)
        return true;

    bool ok = true;

    /* 两阶段安装，理由同 CaptureHooksInstall：绝不能在 g_real* 为空时让钩子生效。 */
    ok = HookAttach("exec", L"kernel32.dll", "CreateProcessW", reinterpret_cast<void*>(&Hook_CreateProcessW), false) && ok;
    ok = HookAttach("exec", L"kernel32.dll", "CreateProcessA", reinterpret_cast<void*>(&Hook_CreateProcessA), false) && ok;
    ok = HookAttach("exec", L"kernel32.dll", "WinExec", reinterpret_cast<void*>(&Hook_WinExec), false) && ok;
    ok = HookAttach("exec", L"user32.dll", "ExitWindowsEx", reinterpret_cast<void*>(&Hook_ExitWindowsEx), false) && ok;

    /* shell32 只在目标进程已经加载它时才挂：不为了审计去加载一个原本没有的模块 */
    bool shellLoaded = (GetModuleHandleW(L"shell32.dll") != nullptr);
    if (shellLoaded)
    {
        HookAttach("exec", L"shell32.dll", "ShellExecuteExW",
                   reinterpret_cast<void*>(&Hook_ShellExecuteExW), false);
    }

    g_realCreateProcessW  = reinterpret_cast<PFN_CreateProcessW>(HookGetOriginal(reinterpret_cast<void*>(&Hook_CreateProcessW)));
    g_realCreateProcessA  = reinterpret_cast<PFN_CreateProcessA>(HookGetOriginal(reinterpret_cast<void*>(&Hook_CreateProcessA)));
    g_realWinExec         = reinterpret_cast<PFN_WinExec>(HookGetOriginal(reinterpret_cast<void*>(&Hook_WinExec)));
    g_realExitWindowsEx   = reinterpret_cast<PFN_ExitWindowsEx>(HookGetOriginal(reinterpret_cast<void*>(&Hook_ExitWindowsEx)));
    g_realShellExecuteExW = reinterpret_cast<PFN_ShellExecuteExW>(HookGetOriginal(reinterpret_cast<void*>(&Hook_ShellExecuteExW)));

    if (g_realCreateProcessW == nullptr || g_realCreateProcessA == nullptr ||
        g_realWinExec == nullptr || g_realExitWindowsEx == nullptr ||
        (shellLoaded && g_realShellExecuteExW == nullptr))
    {
        YZLOGE(L"ExecHooksInstall: 原始函数指针不完整，远程执行审计 hook 保持停用");
        return false;
    }

    if (enableNow && !HookSetGroupEnabled("exec", true))
    {
        YZLOGE(L"ExecHooksInstall: 启用远程执行审计 hook 失败");
        return false;
    }

    g_hooksReady = true;
    YZLOGI(L"ExecHooksInstall: 完成 (enableNow=%d shell32=%d)", enableNow ? 1 : 0, shellLoaded ? 1 : 0);
    return ok;
}

void ExecHooksShutdown()
{
    g_hooksReady = false;
}

DWORD ExecAuditCount()
{
    return static_cast<DWORD>(InterlockedCompareExchange(&g_auditCount, 0, 0));
}
} /* namespace yzhook */
