#include "app.h"

#include "payload.h"

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
    return yz::IsYuanzhiInstallPath(path);
}

/* 网管版守卫进程：服务用 /SelfGuard /Protection 拉起 Nmdeputy.exe 做守护与上报，
   不对它注入——它既不是广播窗口宿主，惊动它也更容易触发保护。 */
bool IsGuardProcess(const std::wstring& name)
{
    return _wcsicmp(name.c_str(), L"Nmdeputy.exe") == 0;
}

/* 客户端主进程优先级（窗口宿主之外）：优先 Yistart.exe，其次网管版广播宿主 ExdPaintHelper.exe */
int NamePriority(const std::wstring& name)
{
    if (_wcsicmp(name.c_str(), L"Yistart.exe") == 0)
        return 3;
    if (_wcsicmp(name.c_str(), L"ExdPaintHelper.exe") == 0)
        return 2;
    if (_wcsicmp(name.c_str(), L"TEACHCMD.exe") == 0)
        return 1;
    if (_wcsicmp(name.c_str(), L"PlayerGUI.exe") == 0)
        return 1;
    return 0;
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

/* 注入时最容易被误读的失败：目标进程受保护 / 句柄权限被内核组件剥夺。
   这种情况下 OpenProcess 会成功，但后续 VM/线程操作返回 ACCESS_DENIED。 */
std::wstring AccessDeniedHint(DWORD err)
{
    if (err != ERROR_ACCESS_DENIED)
        return std::wstring();
    return L"（拒绝访问：目标可能受保护或句柄权限被剥夺，先用 YZProbe.exe --access <pid> 诊断）";
}

struct PostMsgCtx
{
    DWORD pid;
    int   posted;
};

BOOL CALLBACK PostNullProc(HWND hwnd, LPARAM lParam)
{
    PostMsgCtx* ctx = reinterpret_cast<PostMsgCtx*>(lParam);
    DWORD       pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != ctx->pid)
        return TRUE;
    if (PostMessageW(hwnd, WM_NULL, 0, 0))
        ctx->posted++;
    return (ctx->posted == 0);      /* 投出一条就够，触发目标线程处理消息 */
}

/* 备用注入：SetWindowsHookEx(WH_GETMESSAGE)。
   由系统把我们的 DLL 映射进目标进程，我们自己不写目标内存，
   因此能绕开“VM 权限被剥夺”这类保护（但绕不开 PPL/受保护进程）。 */
bool InjectHookDllViaMessageHook(const std::wstring& dllPath, DWORD targetPid,
                                 HHOOK* outHook, std::wstring* err)
{
    HMODULE mod = LoadLibraryW(dllPath.c_str());   /* 只为在本进程取到过程地址 */
    if (mod == nullptr)
    {
        if (err != nullptr)
            *err = L"LoadLibraryW 失败: " + yz::Win32ErrorMessage(GetLastError());
        return false;
    }

    FARPROC proc = GetProcAddress(mod, "YZ_HookProc");
    if (proc == nullptr)
    {
        if (err != nullptr)
            *err = L"YZHook.dll 未导出 YZ_HookProc（载荷版本过旧？）";
        return false;
    }

    HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, reinterpret_cast<HOOKPROC>(proc), mod, 0);
    if (hook == nullptr)
    {
        if (err != nullptr)
            *err = L"SetWindowsHookEx 失败: " + yz::Win32ErrorMessage(GetLastError());
        return false;
    }

    /* 投一条无害消息，促使目标线程处理消息队列，从而加载我们的 DLL */
    PostMsgCtx ctx;
    ctx.pid    = targetPid;
    ctx.posted = 0;
    EnumWindows(PostNullProc, reinterpret_cast<LPARAM>(&ctx));

    *outHook = hook;
    return true;
}

/* 与 YZHook 侧 IsCandidateWindow 用同一套判据，只是这里看的是别人的窗口：
   无属主 + 无标题栏 + 非子窗口 + 非桌面壳类 + 覆盖整块显示器 + (POPUP 或置顶)。 */
bool LooksBroadcastWindow(HWND hwnd)
{
    if (hwnd == nullptr || !IsWindow(hwnd) || !IsWindowVisible(hwnd))
        return false;
    if (GetWindow(hwnd, GW_OWNER) != nullptr)
        return false;

    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    if ((style & WS_CHILD) != 0)
        return false;
    if ((style & WS_CAPTION) == WS_CAPTION)
        return false;

    wchar_t cls[128] = {0};
    if (GetClassNameW(hwnd, cls, 128) != 0)
    {
        static const wchar_t* const kSkip[] =
        {
            L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd",
            L"Button", L"Static", L"#32770"
        };
        for (size_t i = 0; i < sizeof(kSkip) / sizeof(kSkip[0]); i++)
        {
            if (_wcsicmp(cls, kSkip[i]) == 0)
                return false;
        }
    }

    const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((style & WS_POPUP) == 0 && (ex & WS_EX_TOPMOST) == 0)
        return false;

    RECT rc;
    if (!GetWindowRect(hwnd, &rc))
        return false;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi))
        return false;

    const int tol = 3;
    return rc.left <= mi.rcMonitor.left + tol && rc.top <= mi.rcMonitor.top + tol &&
           rc.right >= mi.rcMonitor.right - tol && rc.bottom >= mi.rcMonitor.bottom - tol;
}

struct WindowSearch
{
    DWORD pid;
    int   area;      /* 多个候选时取面积最大的那个 */
};

BOOL CALLBACK BroadcastWindowProc(HWND hwnd, LPARAM lParam)
{
    WindowSearch* s = reinterpret_cast<WindowSearch*>(lParam);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId())
        return TRUE;
    if (!LooksBroadcastWindow(hwnd))
        return TRUE;

    RECT rc;
    if (!GetWindowRect(hwnd, &rc))
        return TRUE;
    const int area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > s->area)
    {
        s->area = area;
        s->pid  = pid;
    }
    return TRUE;
}

/* 找出正在全屏广播的窗口宿主 PID。
   只认身份明确的进程（路径像远志，或名字在注入名单里），否则一个无关的全屏
   播放器/游戏会被误判成"广播窗口"并把我们引到错误的进程上。 */
DWORD FindBroadcastWindowPid(const AppConfig& cfg)
{
    WindowSearch s;
    s.pid  = 0;
    s.area = 0;
    EnumWindows(BroadcastWindowProc, reinterpret_cast<LPARAM>(&s));
    if (s.pid == 0)
        return 0;

    const std::wstring path = yz::GetProcessImagePath(s.pid);
    const std::wstring name = yz::FileNameOf(path);
    if (PathLooksYuanzhi(cfg, path) || NameInList(cfg, name))
        return s.pid;
    return 0;
}

/* 兜底识别：进程里加载了远志客户端模块（Rmdesk.ads / PlayerGUI.dll / ExdHooks.dll）。
   代价是每个进程一次 Toolhelp 快照，所以只在"名字与路径都没命中"时才调用。 */
bool ProcessHasYuanzhiModule(DWORD pid, std::wstring* whichOut)
{
    static const wchar_t* const kModules[] = { L"Rmdesk.ads", L"PlayerGUI.dll", L"ExdHooks.dll" };

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W me;
    ZeroMemory(&me, sizeof(me));
    me.dwSize = sizeof(me);

    bool found = false;
    if (Module32FirstW(snap, &me))
    {
        do
        {
            for (size_t i = 0; i < sizeof(kModules) / sizeof(kModules[0]); i++)
            {
                if (_wcsicmp(me.szModule, kModules[i]) == 0)
                {
                    if (whichOut != nullptr)
                        *whichOut = me.szModule;
                    found = true;
                    break;
                }
            }
            if (found)
                break;
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}
} /* namespace */

std::wstring ResolveHookDllPath()
{
    /* 显式配置优先：留给"用外部 DLL 顶替内嵌载荷"的调试场景。 */
    if (!g_app.cfg.hookDllPath.empty())
    {
        g_app.hookDllSource = L"ini";
        g_app.hookDllError.clear();
        return g_app.cfg.hookDllPath;
    }

    std::wstring err;
    std::wstring extracted = PayloadEnsureHookDll(&err);
    if (!extracted.empty())
    {
        g_app.hookDllSource = L"embedded";
        g_app.hookDllError.clear();
        return extracted;
    }

    /* 内嵌释放失败（磁盘只读、还原卡拦截等）时退回旧的同目录文件方式，
       让开发机上"dist 里直接放 YZHook.dll"的用法继续可用。 */
    g_app.hookDllSource = L"filedir";
    g_app.hookDllError  = err;
    YZLOGW(L"内嵌 Hook 释放失败，回退到同目录 YZHook.dll: %s", err.c_str());
    return yz::JoinPath(g_app.exeDir, L"YZHook.dll");
}

DWORD FindTargetProcess(const AppConfig& cfg)
{
    std::vector<ProcInfo> procs;
    EnumAllProcesses(procs);

    /* 第一路：正在全屏广播的窗口宿主。
       教师端常规广播由 ExdHooks 驱动、窗口很可能由 ExdPaintHelper.exe 承载，
       所以这一路权重最高：把 Hook 注进窗口宿主，窗口化才作用在真正的广播窗口上。 */
    const DWORD windowPid = FindBroadcastWindowPid(cfg);

    DWORD        best        = 0;
    int          bestScore   = 0;
    std::wstring bestName;
    std::wstring bestReason;

    /* 网管版：守卫进程在场时提示一次（它可能重启客户端并重新保护），但不注入它 */
    static bool s_guardLogged = false;
    if (!s_guardLogged)
    {
        for (size_t i = 0; i < procs.size(); i++)
        {
            if (!IsGuardProcess(procs[i].name))
                continue;
            s_guardLogged = true;
            YZLOGI(L"检测到网管版守卫进程 %s (pid=%u)：只注入客户端，不碰守卫",
                   procs[i].name.c_str(), procs[i].pid);
            break;
        }
    }

    for (size_t i = 0; i < procs.size(); i++)
    {
        if (IsGuardProcess(procs[i].name))
            continue;

        int score = 0;
        std::wstring reason;

        if (PathLooksYuanzhi(cfg, procs[i].path))
        {
            score += 2;
            reason += L"安装路径 ";
        }
        if (NameInList(cfg, procs[i].name))
        {
            score += 3;
            reason += L"进程名 ";
        }
        const int prio = NamePriority(procs[i].name);
        if (prio != 0)
        {
            score += prio;
            reason += L"主进程优先 ";
        }
        if (windowPid != 0 && procs[i].pid == windowPid)
        {
            score += 5;
            reason += L"广播窗口宿主 ";
        }

        if (score > bestScore)
        {
            bestScore  = score;
            best       = procs[i].pid;
            bestName   = procs[i].name;
            bestReason = reason;
        }
    }

    if (bestScore >= 3 && best != 0)
    {
        /* 目标变化时才记一行，避免每 2 秒刷屏 */
        static DWORD        s_lastLoggedPid = 0;
        static std::wstring s_lastLoggedName;
        if (best != s_lastLoggedPid || bestName != s_lastLoggedName)
        {
            s_lastLoggedPid  = best;
            s_lastLoggedName = bestName;
            YZLOGI(L"目标进程 pid=%u (%s) 命中: %s", best, bestName.c_str(), bestReason.c_str());
        }
        return best;
    }

    /* 第二路（兜底，代价高）：谁加载了远志客户端模块就注谁。
       只有在进程名与安装路径都没识别出来时才走这里。 */
    for (size_t i = 0; i < procs.size(); i++)
    {
        if (IsGuardProcess(procs[i].name))
            continue;

        std::wstring which;
        if (!ProcessHasYuanzhiModule(procs[i].pid, &which))
            continue;
        YZLOGI(L"目标进程 pid=%u (%s) 兜底命中: 已加载 %s",
               procs[i].pid, procs[i].name.c_str(), which.c_str());
        return procs[i].pid;
    }

    return 0;
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
        {
            const DWORD werr = GetLastError();
            *err = L"VirtualAllocEx 失败: " + yz::Win32ErrorMessage(werr) + AccessDeniedHint(werr);
        }
        CloseHandle(process);
        return false;
    }

    bool ok = false;
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, dllPath.c_str(), bytes, &written))
    {
        if (err != nullptr)
        {
            const DWORD werr = GetLastError();
            *err = L"WriteProcessMemory 失败: " + yz::Win32ErrorMessage(werr) + AccessDeniedHint(werr);
        }
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
                {
                    const DWORD werr = GetLastError();
                    *err = L"CreateRemoteThread 失败: " + yz::Win32ErrorMessage(werr) + AccessDeniedHint(werr);
                }
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

    /* 备用注入方式：消息钩子。挂上之后就不再重复挂，等目标连上管道。 */
    if (g_app.cfg.injectMethod == 1)
    {
        if (g_app.injectHook != nullptr)
            return;

        std::wstring err;
        HHOOK        hook = nullptr;
        if (InjectHookDllViaMessageHook(ResolveHookDllPath(), pid, &hook, &err))
        {
            g_app.injectHook = hook;
            g_app.injectCount++;
            YZLOGI(L"已通过消息钩子注入 HHOOK=%p pid=%u", hook, pid);
            UiAppendLog(yz::kLogInfo, yz::Format(L"已通过消息钩子注入目标进程 PID=%u", pid));
        }
        else
        {
            YZLOGW(L"消息钩子注入失败: %s", err.c_str());
            UiAppendLog(yz::kLogWarn, L"消息钩子注入失败: " + err);
        }
        return;
    }

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

