//
// 诊断包导出：把日志、配置、进程/窗口/服务快照复制到一个目录，方便带回分析。
//
#include "app.h"

#include "yz_log.h"
#include "yz_util.h"

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
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_TYPE_DRIVER | SERVICE_TYPE_WIN32,
                          SERVICE_STATE_ALL, nullptr, 0, &needed, &returned, &resume, nullptr);
    if (needed != 0)
    {
        std::vector<BYTE> buffer(needed);
        if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_TYPE_DRIVER | SERVICE_TYPE_WIN32,
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

void CopyIfExists(const std::wstring& src, const std::wstring& dstDir)
{
    if (!yz::FileExists(src))
        return;
    std::wstring dst = yz::JoinPath(dstDir, yz::FileNameOf(src));
    CopyFileW(src.c_str(), dst.c_str(), FALSE);
}
} /* namespace */

void ExportDiagnostics(HWND owner)
{
    std::wstring dir = yz::JoinPath(g_app.exeDir, L"diag-" + yz::NowStamp());
    if (!yz::EnsureDirectory(dir))
    {
        MessageBoxW(owner, L"无法创建诊断目录。", L"YZTrainer", MB_OK | MB_ICONERROR);
        return;
    }

    /* 日志 */
    CopyIfExists(yz::LogFilePath(), dir);
    CopyIfExists(yz::JoinPath(yz::LogDir(), L"yzt-1.log"), dir);
    CopyIfExists(yz::JoinPath(yz::LogDir(), L"yzt-2.log"), dir);

    /* 上次探针报告 */
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(yz::JoinPath(g_app.exeDir, L"YZProbe-report-*.*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            CopyIfExists(yz::JoinPath(g_app.exeDir, fd.cFileName), dir);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    /* 快照 */
    std::wstring snapshot;
    snapshot += L"YZTrainer 诊断快照\r\n生成时间: " + yz::NowStampEx() + L"\r\n\r\n";
    snapshot += yz::Format(L"== 配置 ==\r\nFlags=0x%08X WindowPercent=%u AutoInject=%d\r\n"
                           L"TargetDir=%s\r\nHookDll=%s\r\nExeDir=%s\r\n\r\n",
                           g_app.cfg.flags, g_app.cfg.windowPercent,
                           g_app.cfg.autoInject ? 1 : 0,
                           g_app.cfg.targetDir.c_str(),
                           ResolveHookDllPath().c_str(),
                           g_app.exeDir.c_str());

    DWORD clientPid = 0;
    bool connected = IpcIsConnected(&clientPid);
    snapshot += yz::Format(L"== 运行状态 ==\r\n连接=%d 客户端PID=%u 目标PID=%u 注入次数=%u 考试模式=%d\r\n"
                           L"Hook数=%u 窗口化次数=%u\r\n主机路径=%s\r\n\r\n",
                           connected ? 1 : 0, clientPid, g_app.targetPid, g_app.injectCount,
                           g_app.examMode ? 1 : 0,
                           g_app.status.hooksInstalled, g_app.status.windowizeCount,
                           g_app.status.hostPath);
    snapshot += ProcessDump();
    snapshot += L"\r\n";
    snapshot += WindowDump(g_app.targetPid != 0 ? g_app.targetPid : clientPid);
    snapshot += L"\r\n";
    snapshot += ServiceDump();

    WriteTextFile(yz::JoinPath(dir, L"snapshot.txt"), snapshot);

    UiAppendLog(yz::kLogInfo, L"诊断包已导出: " + dir);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
