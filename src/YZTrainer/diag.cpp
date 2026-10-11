//
// 诊断包导出：把日志、配置、进程/窗口/服务快照复制到一个目录，方便带回分析。
//
#include "app.h"

#include "capturewatch.h"
#include "payload.h"

#include "yz_log.h"
#include "yz_util.h"

#include "winfix.h"

#include <tlhelp32.h>
#include <winsvc.h>

namespace
{
void WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::string utf8 = yz::WideToUtf8(text);
    DWORD written = 0;
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
}

std::wstring ProcessDump()
{
    std::wstring out = L"== 进程 ==\r\n";
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return out;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            std::wstring path = yz::GetProcessImagePath(pe.th32ProcessID);
            bool interesting = path.empty() ||
                               yz::ContainsNoCase(path, L"YZinfo") ||
                               yz::ContainsNoCase(path, L"GZYZ") ||
                               yz::ContainsNoCase(pe.szExeFile, L"Yistart") ||
                               yz::ContainsNoCase(pe.szExeFile, L"TEACHCMD") ||
                               yz::ContainsNoCase(pe.szExeFile, L"PlayerGUI") ||
                               yz::ContainsNoCase(pe.szExeFile, L"KeyboardHook") ||
                               yz::ContainsNoCase(pe.szExeFile, L"YZTrainer");
            if (interesting)
            {
                out += yz::Format(L"PID=%-6u %-28s %s\r\n", pe.th32ProcessID,
                                  pe.szExeFile, path.c_str());
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

struct WindowDumpContext
{
    DWORD pidFilter;
    std::wstring text;
};

BOOL CALLBACK WindowDumpProc(HWND hwnd, LPARAM lParam)
{
    WindowDumpContext* ctx = reinterpret_cast<WindowDumpContext*>(lParam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (ctx->pidFilter != 0 && pid != ctx->pidFilter)
        return TRUE;

    wchar_t cls[128] = {0};
    wchar_t title[256] = {0};
    GetClassNameW(hwnd, cls, 128);
    GetWindowTextW(hwnd, title, 256);
    RECT rc;
    GetWindowRect(hwnd, &rc);
    LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);

    ctx->text += yz::Format(L"HWND=0x%p PID=%u class=%s style=0x%08X ex=0x%08X rect=%d,%d,%d,%d visible=%d title=%s\r\n",
                            hwnd, pid, cls, style, ex,
                            rc.left, rc.top, rc.right, rc.bottom,
                            IsWindowVisible(hwnd) ? 1 : 0, title);
    return TRUE;
}

std::wstring WindowDump(DWORD pid)
{
    WindowDumpContext ctx;
    ctx.pidFilter = pid;
    ctx.text = L"== 窗口 ==\r\n";
    EnumWindows(WindowDumpProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.text;
}

std::wstring ServiceDump()
{
    std::wstring out = L"== 服务/驱动 ==\r\n";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (scm == nullptr)
    {
        out += L"（无法枚举：需要管理员权限）\r\n";
        return out;
    }

    DWORD needed = 0, returned = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER | SERVICE_WIN32,
                          SERVICE_STATE_ALL, nullptr, 0, &needed, &returned, &resume, nullptr);
    if (needed != 0)
    {
        std::vector<BYTE> buffer(needed);
        if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER | SERVICE_WIN32,
                                  SERVICE_STATE_ALL, buffer.data(), needed, &needed, &returned,
                                  &resume, nullptr))
        {
            ENUM_SERVICE_STATUS_PROCESSW* items =
                reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
            for (DWORD i = 0; i < returned; i++)
            {
                std::wstring name = items[i].lpServiceName != nullptr ? items[i].lpServiceName : L"";
                std::wstring display = items[i].lpDisplayName != nullptr ? items[i].lpDisplayName : L"";
                std::wstring path;
                SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_CONFIG);
                if (svc != nullptr)
                {
                    DWORD cfgSize = 0;
                    QueryServiceConfigW(svc, nullptr, 0, &cfgSize);
                    if (cfgSize != 0)
                    {
                        std::vector<BYTE> cfgBuf(cfgSize);
                        QUERY_SERVICE_CONFIGW* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfgBuf.data());
                        if (QueryServiceConfigW(svc, cfg, cfgSize, &cfgSize) && cfg->lpBinaryPathName != nullptr)
                            path = cfg->lpBinaryPathName;
                    }
                    CloseServiceHandle(svc);
                }
                bool interesting = yz::ContainsNoCase(path, L"GZYZ") ||
                                   yz::ContainsNoCase(path, L"YZinfo") ||
                                   yz::ContainsNoCase(name, L"exdmirr") ||
                                   yz::ContainsNoCase(name, L"videfake") ||
                                   yz::ContainsNoCase(name, L"NdisNetFilter") ||
                                   yz::ContainsNoCase(name, L"nfndis") ||
                                   yz::ContainsNoCase(name, L"ExFilter");
                if (interesting)
                {
                    out += yz::Format(L"%-24s state=%u %s  %s\r\n", name.c_str(),
                                      items[i].ServiceStatusProcess.dwCurrentState,
                                      display.c_str(), path.c_str());
                }
            }
        }
    }
    CloseServiceHandle(scm);
    return out;
}

/* 复制失败必须**说出来**：v0.6.8 起自动导出成了主证据通道，如果某个文件没拷进去
   却只写一句"诊断包已导出"，那是最坏的一种失败——包看着有、证据少一半。
   failures 收进快照，同时记一条 WARN 到日志。 */
void CopyIfExists(const std::wstring& src, const std::wstring& dstDir, std::wstring* failures)
{
    if (!yz::FileExists(src))
        return;
    const std::wstring dst = yz::JoinPath(dstDir, yz::FileNameOf(src));
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
    {
        const std::wstring msg =
            yz::Format(L"%s (err=%u)", yz::FileNameOf(src).c_str(), GetLastError());
        YZLOGW(L"诊断包复制失败: %s", msg.c_str());
        if (failures != nullptr)
        {
            if (!failures->empty())
                *failures += L"; ";
            *failures += msg;
        }
    }
}

/* 同上，但允许改目标文件名（给"另一处日志目录"的日志加来源后缀用） */
void CopyAsIfExists(const std::wstring& src, const std::wstring& dstDir, const std::wstring& dstName,
                    std::wstring* failures)
{
    if (!yz::FileExists(src))
        return;
    const std::wstring dst = yz::JoinPath(dstDir, dstName);
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
    {
        const std::wstring msg = yz::Format(L"%s (err=%u)", dstName.c_str(), GetLastError());
        YZLOGW(L"诊断包复制失败: %s", msg.c_str());
        if (failures != nullptr)
        {
            if (!failures->empty())
                *failures += L"; ";
            *failures += msg;
        }
    }
}

/* 在扩展名前插入来源后缀：yzt-1.log + "-sys" → yzt-1-sys.log */
std::wstring WithSuffix(const std::wstring& name, const std::wstring& suffix)
{
    if (suffix.empty())
        return name;
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos)
        return name + suffix;
    return name.substr(0, dot) + suffix + name.substr(dot);
}

/* 把某个日志目录里的 yzt*.log 与 remote-exec*.log 全部收进诊断包。
   v0.6.29 起不再只认 -1/-2 两个固定名字（轮转更多次就漏），而是按通配枚举；
   并且会把**其它来源**（SYSTEM 的 temp、各用户 profile 下的 temp）的日志也带上，
   用后缀标出来源——一趟上机里用户身份与 SYSTEM 身份都可能跑过，
   只带自己那一份，恰恰就是"时间匆忙时漏采"的来源。 */
void CollectLogsFrom(const std::wstring& logDir, const std::wstring& dstDir,
                     const std::wstring& suffix, std::wstring* failures)
{
    if (logDir.empty() || !yz::DirectoryExists(logDir))
        return;

    const wchar_t* patterns[] = { L"yzt*.log", L"remote-exec*.log" };
    for (const wchar_t* pattern : patterns)
    {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW(yz::JoinPath(logDir, pattern).c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE)
            continue;
        do
        {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                continue;
            CopyAsIfExists(yz::JoinPath(logDir, fd.cFileName), dstDir,
                           WithSuffix(fd.cFileName, suffix), failures);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
}
} /* namespace */

/* 真正干活的：建目录 + 收日志/探针报告 + 生成快照。不做 UI（不弹窗、不开资源管理器），
   这样"自动导出"这条路径可以完全静默地从看门狗线程/退出路径上调用。 */
static bool BuildDiagnostics(const std::wstring& dir, const wchar_t* reason)
{
    if (!yz::EnsureDirectory(dir))
        return false;

    /* 日志：自己的那一份用原名；**其它来源**（SYSTEM 的 temp、各用户 profile 下的 temp）
       带上来源后缀一起进包（v0.6.29）——一趟上机里两个身份都可能跑过，只带自己那份
       就是"时间匆忙时漏采"的最大来源。 */
    std::wstring copyFailures;
    std::wstring logSources;
    const std::wstring myLogDir = yz::LogDir();
    CollectLogsFrom(myLogDir, dir, L"", &copyFailures);
    logSources += L"own=" + myLogDir;

    /* SYSTEM 的 temp（我们自己就是 SYSTEM 时与 myLogDir 相同，CollectLogsFrom 会自己跳过不存在） */
    const std::wstring sysLogDir = yz::JoinPath(L"C:\\Windows\\Temp", L"YZTrainer");
    if (_wcsicmp(sysLogDir.c_str(), myLogDir.c_str()) != 0)
    {
        CollectLogsFrom(sysLogDir, dir, L"-sys", &copyFailures);
        if (yz::DirectoryExists(sysLogDir))
            logSources += L"; sys=" + sysLogDir;
    }

    /* 各用户的 temp：C:\Users\<name>\AppData\Local\Temp\YZTrainer（最多取 10 个，避免异常环境里扫太久） */
    {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW(L"C:\\Users\\*", &fd);
        int users = 0;
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 || users >= 10)
                    continue;
                if (fd.cFileName[0] == L'.')
                    continue;
                const std::wstring userLogDir = yz::JoinPath(
                    yz::JoinPath(yz::JoinPath(yz::JoinPath(L"C:\\Users", fd.cFileName),
                                             L"AppData"), L"Local"), L"Temp\\YZTrainer");
                if (_wcsicmp(userLogDir.c_str(), myLogDir.c_str()) != 0 &&
                    _wcsicmp(userLogDir.c_str(), sysLogDir.c_str()) != 0)
                {
                    CollectLogsFrom(userLogDir, dir, std::wstring(L"-") + fd.cFileName, &copyFailures);
                    if (yz::DirectoryExists(userLogDir))
                    {
                        logSources += std::wstring(L"; user=") + fd.cFileName;
                        ++users;
                    }
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
    }
    /* 远程执行审计同样由 CollectLogsFrom 一并收了（它按 remote-exec*.log 通配，不再只认两个名字） */

    /* 上次探针报告 */
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(yz::JoinPath(g_app.exeDir, L"YZProbe-report-*.*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            CopyIfExists(yz::JoinPath(g_app.exeDir, fd.cFileName), dir, &copyFailures);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    /* 快照 */
    std::wstring snapshot;
    snapshot += L"YZTrainer 诊断快照\r\n生成时间: " + yz::NowStampEx() + L"\r\n";
    snapshot += yz::Format(L"导出方式: %s\r\n\r\n", (reason != nullptr) ? reason : L"未知");
    if (!copyFailures.empty())
        snapshot += L"**复制失败（这份诊断包不完整）**: " + copyFailures + L"\r\n\r\n";
    snapshot += L"== 日志来源 ==\r\n" + logSources + L"\r\n（own=本进程日志目录；-sys 后缀=SYSTEM 的 temp；"
                L"-<用户名> 后缀=该用户 profile 下的 temp；只有存在过的来源才会列在这里）\r\n\r\n";
    snapshot += yz::Format(L"== 配置 ==\r\nFlags=0x%08X WindowPercent=%u AutoInject=%d EnableExamGuard=%d "
                           L"InjectMethod=%d ExternalWindowFix=%d\r\n"
                           L"假全屏=%d（优先于窗口化）\r\n"
                           L"TargetDir=%s\r\nHookDll=%s\r\n"
                           L"HookDllSource=%s 内嵌资源=%u 字节\r\nExeDir=%s\r\n\r\n",
                           g_app.cfg.flags, g_app.cfg.windowPercent,
                           g_app.cfg.autoInject ? 1 : 0,
                           g_app.cfg.enableExamGuard ? 1 : 0,
                           g_app.cfg.injectMethod,
                           g_app.cfg.externalWindowFix ? 1 : 0,
                           (g_app.cfg.flags & YZ_FLAG_FAKE_FULLSCREEN) ? 1 : 0,
                           g_app.cfg.targetDir.c_str(),
                           ResolveHookDllPath().c_str(),
                           g_app.hookDllSource.c_str(), PayloadEmbeddedSize(),
                           g_app.exeDir.c_str());
    if (!g_app.hookDllError.empty())
        snapshot += L"HookDllReleaseError=" + g_app.hookDllError + L"\r\n\r\n";

    DWORD clientPid = 0;
    bool connected = IpcIsConnected(&clientPid);
    DWORD fixCount = 0;
    DWORD fixCandidates = 0;
    bool  fixFailed = false;
    winfix::WinFixStats(&fixCount, &fixCandidates, &fixFailed);
    snapshot += yz::Format(L"== 运行状态 ==\r\n连接=%d 客户端PID=%u 目标PID=%u 注入次数=%u 考试模式=%d\r\n"
                           L"Hook数=%u 窗口化次数=%u\r\n"
                           L"外部窗口纠正: 次数=%u 上轮候选=%u 有写入失败=%d\r\n"
                           L"主机路径=%s\r\n\r\n",
                           connected ? 1 : 0, clientPid, g_app.targetPid, g_app.injectCount,
                           g_app.examMode ? 1 : 0,
                           g_app.status.hooksInstalled, g_app.status.windowizeCount,
                           fixCount, fixCandidates, fixFailed ? 1 : 0,
                           g_app.status.hostPath);
    snapshot += ProcessDump();
    snapshot += L"\r\n";
    /* 采集栈快照：冻结画面要按"教师监视时谁在抓屏"选钩子路径，而那一刻极难人工复现，
       所以常驻监控每 30 秒扫一次，把结果一并写进每份诊断包。 */
    snapshot += capturewatch::Dump();
    snapshot += WindowDump(g_app.targetPid != 0 ? g_app.targetPid : clientPid);
    snapshot += L"\r\n";
    snapshot += ServiceDump();

    WriteTextFile(yz::JoinPath(dir, L"snapshot.txt"), snapshot);
    return true;
}

void ExportDiagnostics(HWND owner)
{
    const std::wstring dir = yz::JoinPath(g_app.exeDir, L"diag-" + yz::NowStamp());
    if (!BuildDiagnostics(dir, L"手动导出（托盘菜单）"))
    {
        MessageBoxW(owner, L"无法创建诊断目录。", L"YZTrainer", MB_OK | MB_ICONERROR);
        return;
    }
    UiAppendLog(yz::kLogInfo, L"诊断包已导出: " + dir);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void AutoExportDiagnostics(const wchar_t* reason)
{
    /* 名字带 -auto 便于一眼区分；NowStampEx 带毫秒，避免同一秒内两次导出撞名 */
    std::wstring dir = yz::JoinPath(g_app.exeDir, L"diag-" + yz::NowStampEx() + L"-auto");
    bool fellBack = false;
    if (!BuildDiagnostics(dir, reason))
    {
        /* v0.6.29：exeDir 不可写（只读介质、受限 ACL 等）时**回落到日志目录**再试一次。
           以前这种情况只记一条 WARN，日志就只剩 temp 里那份——现场匆忙时最容易漏。 */
        const std::wstring alt = yz::JoinPath(yz::LogDir(), L"diag-" + yz::NowStampEx() + L"-auto");
        if (BuildDiagnostics(alt, reason))
        {
            YZLOGW(L"诊断包无法写入程序目录（%s），已改写到日志目录: %s", g_app.exeDir.c_str(), alt.c_str());
            UiAppendLog(yz::kLogWarn,
                        yz::Format(L"诊断包已改写到日志目录（程序目录不可写）: %s", alt.c_str()));
            dir = alt;
            fellBack = true;
        }
        else
        {
            YZLOGW(L"自动导出诊断包失败（%s）：程序目录与日志目录都不可写", (reason != nullptr) ? reason : L"未知");
            return;
        }
    }
    if (fellBack)
    {
        YZLOGI(L"自动导出诊断包（%s，已回落）: %s", (reason != nullptr) ? reason : L"未知", dir.c_str());
        return;
    }
    YZLOGI(L"自动导出诊断包（%s）: %s", (reason != nullptr) ? reason : L"未知", dir.c_str());
    UiAppendLog(yz::kLogInfo,
                yz::Format(L"自动导出诊断包（%s）: %s", (reason != nullptr) ? reason : L"未知", dir.c_str()));
}

void AutoExportOnBroadcastOnce(const wchar_t* evidence)
{
    if (!g_app.cfg.autoExportOnBroadcast || g_app.autoExportedBroadcast)
        return;
    g_app.autoExportedBroadcast = true;      /* 先置位：导出本身失败也不要反复重试 */
    AutoExportDiagnostics((evidence != nullptr) ? evidence : L"首次识别到广播窗口");
}

void AutoExportOnCaptureOnce(const wchar_t* evidence)
{
    if (!g_app.cfg.autoExportOnCapture || g_app.autoExportedCapture)
        return;
    g_app.autoExportedCapture = true;
    AutoExportDiagnostics((evidence != nullptr) ? evidence : L"首次发现采集组件");
}
