//
// YZSysRun.exe —— 以 SYSTEM 身份运行指定程序的轻量启动器
//
// 用途：验证"学生端以 SYSTEM 运行、而我们从高完整性进程打开它的句柄会被削掉 VM 权限"
//       这条推断到底是身份问题还是驱动针对进程的问题。做法是把 YZProbe 以 SYSTEM 身份
//       再跑一次，对比两次的逐权限位实测结果。
//
// 为什么不用 Windows 服务：服务会被强制放进 Session 0，而 Session 0 没有交互桌面。
//       探针在那种环境里既看不到 Session 1 的窗口、也挂不上 GUI 消息队列，数据完全失真。
//       本工具从**当前会话**的 winlogon.exe 复制主令牌，并以 lpDesktop = winsta0\default
//       在同一会话、同一桌面里启动目标进程，因此目标进程看到的环境与手边的桌面一致。
//
// 为什么不需要 SeTcbPrivilege：只取本会话的 winlogon 令牌，它的 SessionId 已经等于
//       当前会话，所以不需要 SetTokenInformation(TokenSessionId)——那个调用才需要
//       SeTcbPrivilege。只有显式用 --session 指定别的会话时才会尝试改写会话 ID，
//       失败时如实报告，不会静默降级成 Session 0。
//
// 不装服务、不写注册表、不加持久化项：进程退出即结束。
//
// 用法：
//   YZSysRun.exe [--no-wait] [--session N] -- <程序> [参数...]
//   YZSysRun.exe [--no-wait] <程序> [参数...]
// 例：
//   YZSysRun.exe -- YZProbe.exe -o C:\probe-out --no-pause
//
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <tlhelp32.h>
#include <userenv.h>

#include <string>
#include <vector>

#include "yz_protocol.h"
#include "yz_util.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "userenv.lib")

namespace
{
/* ---------- 输出：控制台优先走 WriteConsoleW，避免重定向/代码页把中文变成 '?' ---------- */
void Out(const wchar_t* fmt, ...)
{
    wchar_t buf[4096] = {0};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, args);
    va_end(args);

    HANDLE out  = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    if (out != nullptr && out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode))
    {
        DWORD written = 0;
        WriteConsoleW(out, buf, static_cast<DWORD>(wcslen(buf)), &written, nullptr);
    }
    else if (out != nullptr && out != INVALID_HANDLE_VALUE)
    {
        const std::string utf8    = yz::WideToUtf8(buf);
        DWORD             written = 0;
        WriteFile(out, utf8.c_str(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
}

const wchar_t* IntegrityText(DWORD rid)
{
    switch (rid)
    {
    case 0x00004000: return L"系统(16384)";
    case 0x00003000: return L"高(12288)";
    case 0x00002000: return L"中(8192)";
    case 0x00001000: return L"低(4096)";
    case 0x00000000: return L"读取失败";
    default:         return L"未知";
    }
}

DWORD TokenIntegrityRid(HANDLE token)
{
    DWORD ret = 0;
    if (!GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &ret) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return 0;

    std::vector<BYTE> buf(ret == 0 ? 128 : ret);
    if (!GetTokenInformation(token, TokenIntegrityLevel, buf.data(),
                             static_cast<DWORD>(buf.size()), &ret))
        return 0;

    TOKEN_MANDATORY_LABEL* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
    if (label->Label.Sid == nullptr)
        return 0;
    const DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
    return count > 0 ? *GetSidSubAuthority(label->Label.Sid, count - 1) : 0;
}

std::wstring TokenUserName(HANDLE token)
{
    DWORD ret = 0;
    if (!GetTokenInformation(token, TokenUser, nullptr, 0, &ret) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return L"未知";

    std::vector<BYTE> buf(ret == 0 ? 128 : ret);
    if (!GetTokenInformation(token, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &ret))
        return L"未知";

    TOKEN_USER*  tu        = reinterpret_cast<TOKEN_USER*>(buf.data());
    wchar_t      name[256] = {0};
    wchar_t      domain[256] = {0};
    DWORD        nameLen   = ARRAYSIZE(name);
    DWORD        domainLen = ARRAYSIZE(domain);
    SID_NAME_USE use;
    if (!LookupAccountSidW(nullptr, tu->User.Sid, name, &nameLen, domain, &domainLen, &use))
        return yz::Format(L"<SID 解析失败 err=%u>", GetLastError());
    return yz::Format(L"%s\\%s", domain, name);
}

/* 名字避开 TOKEN_INFORMATION_CLASS 里的 TokenSessionId（同名会撞枚举）。 */
DWORD SessionIdOfToken(HANDLE token)
{
    DWORD session = 0xFFFFFFFFu;
    DWORD ret     = 0;
    if (!GetTokenInformation(token, TokenSessionId, &session, sizeof(session), &ret))
        return 0xFFFFFFFFu;
    return session;
}

/* 只找**指定会话**里的 winlogon：SYSTEM 令牌本身不区分会话，会话错了就前功尽弃。 */
DWORD FindWinlogonInSession(DWORD session)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);

    DWORD found = 0;
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (_wcsicmp(pe.szExeFile, L"winlogon.exe") != 0)
                continue;
            DWORD s = 0xFFFFFFFFu;
            if (!ProcessIdToSessionId(pe.th32ProcessID, &s))
                continue;
            if (s == session)
            {
                found = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

std::wstring QuoteArg(const std::wstring& s)
{
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos)
        return s;

    std::wstring out = L"\"";
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == L'"')
            out += L'\\';
        out += s[i];
    }
    out += L"\"";
    return out;
}

void Usage()
{
    Out(L"YZSysRun %s —— 以 SYSTEM 身份运行指定程序（不装服务、不写注册表）\n\n", YZ_VERSION_STR);
    Out(L"用法: YZSysRun.exe [--no-wait] [--session N] -- <程序> [参数...]\n");
    Out(L"      YZSysRun.exe [--no-wait] <程序> [参数...]\n\n");
    Out(L"  --no-wait    不等待子进程结束\n");
    Out(L"  --session N  从第 N 个会话的 winlogon 取令牌（默认取当前会话）\n");
    Out(L"  --           之后的参数全部交给目标程序\n\n");
    Out(L"说明: 令牌从当前会话的 winlogon.exe 复制，子进程留在同一会话、同一桌面\n");
    Out(L"      (winsta0\\default)，因此仍能操作交互桌面上的窗口。\n");
}
} /* namespace */

int wmain(int argc, wchar_t** argv)
{
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; i++)
        args.push_back(argv[i]);

    bool                      wait    = true;
    bool                      target  = false;   /* 见到第一个非选项参数后全是目标程序与参数 */
    DWORD                     session = yz::SessionIdOfCurrentProcess();
    std::vector<std::wstring> rest;

    for (size_t i = 0; i < args.size(); i++)
    {
        const std::wstring& a = args[i];
        if (!target)
        {
            if (a == L"--")
            {
                target = true;
                continue;
            }
            if (a == L"-h" || a == L"--help")
            {
                Usage();
                return 0;
            }
            if (a == L"--no-wait")
            {
                wait = false;
                continue;
            }
            if (a == L"--session" && i + 1 < args.size())
            {
                session = static_cast<DWORD>(_wtoi(args[++i].c_str()));
                continue;
            }
            target = true;
        }
        rest.push_back(a);
    }

    if (rest.empty())
    {
        Usage();
        return 2;
    }

    const DWORD myPid     = GetCurrentProcessId();
    const DWORD mySession = yz::SessionIdOfCurrentProcess();

    Out(L"YZSysRun %s\n", YZ_VERSION_STR);
    Out(L"本进程 : pid=%u 会话=%u 完整性=%s\n", myPid, mySession,
        IntegrityText(yz::GetHandleIntegrityRid(GetCurrentProcess())));
    Out(L"目标   : %s", rest[0].c_str());
    for (size_t i = 1; i < rest.size(); i++)
        Out(L" %s", rest[i].c_str());
    Out(L"\n");

    const bool seDebug = yz::EnablePrivilege(SE_DEBUG_NAME);
    Out(L"SeDebug  : %s\n", seDebug ? L"已启用" : L"启用失败（需要管理员身份运行）");

    /* CreateProcessAsUserW 的两个前置权限。提权后的管理员令牌里它们存在但默认未启用，
       不启用就会拿到 ERROR_PRIVILEGE_NOT_HELD、白白退到 CreateProcessWithTokenW。 */
    yz::EnablePrivilege(SE_ASSIGNPRIMARYTOKEN_NAME);
    yz::EnablePrivilege(SE_INCREASE_QUOTA_NAME);

    if (session != mySession)
    {
        Out(L"注意    : 请求会话 %u 与当前会话 %u 不同；改写令牌会话需要 SeTcbPrivilege，"
            L"通常拿不到。\n", session, mySession);
    }

    const DWORD winlogonPid = FindWinlogonInSession(session);
    if (winlogonPid == 0)
    {
        Out(L"错误    : 会话 %u 里找不到 winlogon.exe，无法复制 SYSTEM 令牌。\n", session);
        return 3;
    }
    Out(L"令牌来源: winlogon.exe pid=%u（会话 %u）\n", winlogonPid, session);

    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, winlogonPid);
    if (hp == nullptr)
        hp = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, winlogonPid);
    if (hp == nullptr)
    {
        Out(L"错误    : OpenProcess(winlogon) 失败: %s\n",
            yz::Win32ErrorMessage(GetLastError()).c_str());
        return 4;
    }

    HANDLE ht = nullptr;
    if (!OpenProcessToken(hp, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &ht))
    {
        Out(L"错误    : OpenProcessToken 失败: %s\n",
            yz::Win32ErrorMessage(GetLastError()).c_str());
        CloseHandle(hp);
        return 5;
    }
    CloseHandle(hp);

    HANDLE token = nullptr;
    if (!DuplicateTokenEx(ht, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &token))
    {
        Out(L"错误    : DuplicateTokenEx 失败: %s\n",
            yz::Win32ErrorMessage(GetLastError()).c_str());
        CloseHandle(ht);
        return 6;
    }
    CloseHandle(ht);

    Out(L"复制令牌: 用户=%s 会话=%u 完整性=%s\n",
        TokenUserName(token).c_str(), SessionIdOfToken(token), IntegrityText(TokenIntegrityRid(token)));

    if (SessionIdOfToken(token) != mySession)
    {
        DWORD want = mySession;
        if (SetTokenInformation(token, TokenSessionId, &want, sizeof(want)))
            Out(L"会话改写: 已改为 %u\n", mySession);
        else
            Out(L"会话改写: 失败（%s）——子进程会留在令牌原本的会话里。\n",
                yz::Win32ErrorMessage(GetLastError()).c_str());
    }

    /* 命令行：参数原样拼接，工作目录沿用调用者的当前目录。 */
    std::wstring cmdline = QuoteArg(rest[0]);
    for (size_t i = 1; i < rest.size(); i++)
        cmdline += L" " + QuoteArg(rest[i]);

    wchar_t cwd[MAX_PATH * 2] = {0};
    GetCurrentDirectoryW(ARRAYSIZE(cwd), cwd);

    void* env = nullptr;
    if (!CreateEnvironmentBlock(&env, token, FALSE))
        env = nullptr;   /* 拿不到就用调用者环境，不影响本工具的用途 */

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb        = sizeof(si);
    si.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    const DWORD          flags = CREATE_UNICODE_ENVIRONMENT;
    std::vector<wchar_t> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back(L'\0');

    const wchar_t* how = L"CreateProcessAsUserW";
    BOOL ok = CreateProcessAsUserW(token, nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                  flags, env, cwd, &si, &pi);
    if (!ok)
    {
        const DWORD err1 = GetLastError();
        /* 回退：CreateProcessWithTokenW 走 SeImpersonatePrivilege，缺
           SeAssignPrimaryToken 的环境下它更容易成功。 */
        ZeroMemory(&pi, sizeof(pi));
        ok  = CreateProcessWithTokenW(token, 0, nullptr, mutableCmd.data(), flags, env, cwd, &si, &pi);
        how = L"CreateProcessWithTokenW";
        if (!ok)
        {
            Out(L"错误    : 启动失败。CreateProcessAsUserW: %s\n",
                yz::Win32ErrorMessage(err1).c_str());
            Out(L"          CreateProcessWithTokenW: %s\n",
                yz::Win32ErrorMessage(GetLastError()).c_str());
        }
    }

    if (env != nullptr)
        DestroyEnvironmentBlock(env);

    if (!ok)
    {
        CloseHandle(token);
        return 7;
    }
    CloseHandle(token);
    CloseHandle(pi.hThread);

    DWORD childSession = 0xFFFFFFFFu;
    ProcessIdToSessionId(pi.dwProcessId, &childSession);

    HANDLE hc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.dwProcessId);
    if (hc != nullptr)
    {
        const DWORD il = yz::GetHandleIntegrityRid(hc);
        CloseHandle(hc);
        Out(L"已启动 (%s): pid=%u 会话=%u 完整性=%s\n",
            how, pi.dwProcessId, childSession, IntegrityText(il));
        if (il != 0x00004000)
            Out(L"警告    : 子进程不是系统完整性，说明没有真正以 SYSTEM 运行。\n");
    }
    else
    {
        Out(L"已启动 (%s): pid=%u 会话=%u（子进程完整性读取失败）\n",
            how, pi.dwProcessId, childSession);
    }

    if (wait)
    {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        Out(L"子进程退出码 = %u\n", code);
        CloseHandle(pi.hProcess);
        return static_cast<int>(code);
    }

    Out(L"--no-wait: 不等待子进程。\n");
    CloseHandle(pi.hProcess);
    return 0;
}
