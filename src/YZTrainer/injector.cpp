#include "app.h"

#include "yz_log.h"
#include "yz_util.h"

#include <tlhelp32.h>
#include <string.h>

namespace
{
struct ProcInfo
{
    DWORD        pid;
    std::wstring name;
    std::wstring path;
};

void EnumAllProcesses(std::vector<ProcInfo>& out)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            ProcInfo info;
            info.pid  = pe.th32ProcessID;
            info.name = pe.szExeFile;
            if (info.pid != 0 && info.pid != GetCurrentProcessId())
                info.path = yz::GetProcessImagePath(info.pid);
            out.push_back(info);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

bool NameInList(const AppConfig& cfg, const std::wstring& name)
{
    for (size_t i = 0; i < cfg.processNames.size(); i++)
    {
        if (_wcsicmp(cfg.processNames[i].c_str(), name.c_str()) == 0)
            return true;
    }
    return false;
}

bool PathLooksYuanzhi(const AppConfig& cfg, const std::wstring& path)
{
    if (path.empty())
        return false;
    if (!cfg.targetDir.empty() && yz::IsUnderDir(path, cfg.targetDir))
        return true;
    return yz::ContainsNoCase(path, L"YZinfo Multimedia teaching software") ||
           yz::ContainsNoCase(path, L"GZYZ");
}

bool IsProcessX86(DWORD pid, bool* outX86)
{
    *outX86 = true;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
        return false;
    BOOL wow64 = FALSE;
    bool ok = false;
    if (IsWow64Process(h, &wow64))
    {
        *outX86 = wow64 ? true : false;
        ok = true;
    }
    CloseHandle(h);
    return ok;
}
} /* namespace */

std::wstring ResolveHookDllPath()
{
    if (!g_app.cfg.hookDllPath.empty())
        return g_app.cfg.hookDllPath;
    return yz::JoinPath(g_app.exeDir, L"YZHook.dll");
}

DWORD FindTargetProcess(const AppConfig& cfg)
{
    std::vector<ProcInfo> procs;
    EnumAllProcesses(procs);

    DWORD best = 0;
    int   bestScore = 0;

    for (size_t i = 0; i < procs.size(); i++)
    {
        int score = 0;
        if (PathLooksYuanzhi(cfg, procs[i].path))
            score += 2;
        if (NameInList(cfg, procs[i].name))
            score += 3;
        if (score > bestScore)
        {
            bestScore = score;
            best      = procs[i].pid;
        }
    }

    if (bestScore < 3)
        return 0;
    return best;
}

bool InjectHookDll(DWORD pid, const std::wstring& dllPath, std::wstring* err)
{
    if (!yz::FileExists(dllPath))
    {
        if (err != nullptr)
            *err = L"Hook DLL 不存在: " + dllPath;
        return false;
    }

    bool targetX86 = true;
    if (!IsProcessX86(pid, &targetX86))
    {
        if (err != nullptr)
            *err = L"无法查询目标进程架构（可能权限不足）";
        return false;
    }

#ifdef _WIN64
    bool selfX86 = false;
#else
    bool selfX86 = true;
#endif
    if (targetX86 != selfX86)
    {
        if (err != nullptr)
            *err = yz::Format(L"架构不匹配：目标进程 %s，本程序 %s（请使用对应的构建版本）",
                              targetX86 ? L"x86" : L"x64", selfX86 ? L"x86" : L"x64");
        return false;
    }

    yz::EnablePrivilege(SE_DEBUG_NAME);

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                 FALSE, pid);
    if (process == nullptr)
    {
        if (err != nullptr)
            *err = L"OpenProcess 失败: " + yz::Win32ErrorMessage(GetLastError());
        return false;
    }

    const SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr)
    {
        if (err != nullptr)
            *err = L"VirtualAllocEx 失败: " + yz::Win32ErrorMessage(GetLastError());
        CloseHandle(process);
        return false;
    }

    bool ok = false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, &written))
    {
        if (err != nullptr)
            *err = L"WriteProcessMemory 失败: " + yz::Win32ErrorMessage(GetLastError());
    }
    else
    {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        LPTHREAD_START_ROUTINE loadLibrary =
            reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
        if (loadLibrary == nullptr)
        {
            if (err != nullptr)
                *err = L"无法获取 LoadLibraryW 地址";
        }
        else
        {
            HANDLE thread = CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
            if (thread == nullptr)
            {
                if (err != nullptr)
                    *err = L"CreateRemoteThread 失败: " + yz::Win32ErrorMessage(GetLastError());
            }
            else
            {
                DWORD wait = WaitForSingleObject(thread, 5000);
                DWORD moduleBase = 0;
                GetExitCodeThread(thread, &moduleBase);
                CloseHandle(thread);
                if (wait != WAIT_OBJECT_0)
                {
                    if (err != nullptr)
                        *err = L"注入线程等待超时";
                }
                else if (moduleBase == 0)
                {
                    if (err != nullptr)
                        *err = L"目标进程 LoadLibraryW 返回失败（可能被安全软件拦截）";
                }
                else
                {
                    ok = true;
                }
            }
        }
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    return ok;
}

void WatchdogTick()
{
    if (g_app.examMode)
        return;

    DWORD pid = FindTargetProcess(g_app.cfg);
    g_app.targetPid = pid;

    if (pid == 0 || !g_app.cfg.autoInject)
        return;

    DWORD now = GetTickCount();
    if (now - g_app.lastInjectTick < 2000)
        return;

    DWORD clientPid = 0;
    if (IpcIsConnected(&clientPid) && clientPid == pid)
        return;

    g_app.lastInjectTick = now;

    std::wstring err;
    if (InjectHookDll(pid, ResolveHookDllPath(), &err))
    {
        g_app.injectCount++;
        YZLOGI(L"已注入目标进程 pid=%u (第 %u 次)", pid, g_app.injectCount);
        UiAppendLog(yz::kLogInfo, yz::Format(L"已注入目标进程 PID=%u", pid));
    }
    else
    {
        YZLOGW(L"注入 pid=%u 失败: %s", pid, err.c_str());
        UiAppendLog(yz::kLogWarn, yz::Format(L"注入失败: %s", err.c_str()));
    }
}

