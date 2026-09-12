#include "probe.h"

#include "../common/yz_util.h"

#include <iphlpapi.h>
#include <string.h>
#include <tcpmib.h>
#include <udpmib.h>
#include <tlhelp32.h>
#include <winsvc.h>

#include <stdio.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
std::wstring ArchText(DWORD pid)
{
    bool x86 = true;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
        return L"未知";
    BOOL wow64 = FALSE;
    if (IsWow64Process(h, &wow64))
        x86 = wow64 != FALSE;
    CloseHandle(h);
    return x86 ? L"x86" : L"x64";
}

std::wstring SelfArchText()
{
#ifdef _WIN64
    return L"x64";
#else
    return L"x86";
#endif
}

bool CollectModules(DWORD pid, std::vector<ProbeModule>& out)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    MODULEENTRY32W me;
    ZeroMemory(&me, sizeof(me));
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do
        {
            ProbeModule mod;
            mod.name = me.szModule;
            mod.path = me.szExePath;
            out.push_back(mod);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return true;
}

void CollectProcesses(ProbeData& data, bool includeAllModules)
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
            if (pe.th32ProcessID == 0)
                continue;

            ProbeProcess p;
            p.pid        = pe.th32ProcessID;
            p.name       = pe.szExeFile;
            p.path       = yz::GetProcessImagePath(p.pid);
            p.isYuanzhi  = ProbeIsYuanzhiPath(p.path, p.name);
            p.integrity  = 0;
            p.elevated   = false;

            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
            if (hp != nullptr)
            {
                p.user     = yz::GetHandleUser(hp);
                p.integrity = yz::GetHandleIntegrityRid(hp);
                yz::IsProcessHandleElevated(hp, &p.elevated);
                CloseHandle(hp);
            }

            if (!includeAllModules && !p.isYuanzhi && p.pid != GetCurrentProcessId())
            {
                data.processes.push_back(p);
                continue;
            }

            CollectModules(p.pid, p.modules);
            for (size_t i = 0; i < p.modules.size(); i++)
            {
                if (yz::ContainsNoCase(p.modules[i].name, L".ads") ||
                    yz::ContainsNoCase(p.modules[i].name, L"ExdHooks") ||
                    yz::ContainsNoCase(p.modules[i].name, L"KeyboardHook") ||
                    yz::ContainsNoCase(p.modules[i].name, L"PlayerGUI"))
                {
                    p.isYuanzhi = true;
                }
            }

            /* 远志模块的导出表：后续“调用对方自己的解锁入口”时要用 */
            for (size_t i = 0; i < p.modules.size(); i++)
            {
                const std::wstring& modPath = p.modules[i].path;
                if (modPath.empty())
                    continue;
                if (!yz::ContainsNoCase(modPath, L"YZinfo") && !yz::ContainsNoCase(modPath, L"GZYZ"))
                    continue;
                ProbeReadExports(modPath, p.modules[i].exports);
            }

            data.processes.push_back(p);
        } while (Process32NextW(snap, &pe));

    }
    CloseHandle(snap);
}

struct WindowCtx
{
    ProbeData* data;
};

BOOL CALLBACK WindowProc(HWND hwnd, LPARAM lParam)
{
    WindowCtx* ctx = reinterpret_cast<WindowCtx*>(lParam);
    ProbeData* data = ctx->data;

    ProbeWindow w;
    w.hwnd = hwnd;
    w.pid  = 0;
    GetWindowThreadProcessId(hwnd, &w.pid);
    w.processName = yz::GetProcessImagePath(w.pid);
    w.processName = yz::FileNameOf(w.processName);

    wchar_t cls[256] = {0};
    wchar_t title[512] = {0};
    GetClassNameW(hwnd, cls, 256);
    GetWindowTextW(hwnd, title, 512);
    w.className = cls;
    w.title     = title;

    GetWindowRect(hwnd, &w.rect);
    w.style   = GetWindowLongW(hwnd, GWL_STYLE);
    w.exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
    w.topmost = (w.exStyle & WS_EX_TOPMOST) != 0;
    w.visible = IsWindowVisible(hwnd) != FALSE;
    w.borderless = (w.style & WS_CAPTION) != WS_CAPTION;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    ZeroMemory(&w.monitorRect, sizeof(w.monitorRect));
    w.monitorIndex = 0;
    if (GetMonitorInfoW(mon, &mi))
    {
        w.monitorRect = mi.rcMonitor;
        const int tol = 3;
        w.coversMonitor = (w.rect.left <= mi.rcMonitor.left + tol &&
                           w.rect.top <= mi.rcMonitor.top + tol &&
                           w.rect.right >= mi.rcMonitor.right - tol &&
                           w.rect.bottom >= mi.rcMonitor.bottom - tol);
    }
    else
    {
        w.coversMonitor = false;
    }

    std::wstring procPath = yz::GetProcessImagePath(w.pid);
    w.isYuanzhi = ProbeIsYuanzhiPath(procPath, w.processName);

    data->windows.push_back(w);
    return TRUE;
}

void CollectWindows(ProbeData& data)
{
    WindowCtx ctx;
    ctx.data = &data;
    EnumWindows(WindowProc, reinterpret_cast<LPARAM>(&ctx));
}

bool LooksYuanzhiService(const std::wstring& name, const std::wstring& path)
{
    static const wchar_t* kCandidates[] =
    {
        L"exdmirr", L"videfake", L"NdisNetFilter", L"nfndis", L"ExFilter",
        L"DiskFlt", L"UsbFilter", L"GZYZ", L"YZinfo", L"ExdDrvGuard"
    };
    std::wstring lowerName = yz::ToLower(name);
    std::wstring lowerPath = yz::ToLower(path);
    for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); i++)
    {
        std::wstring needle = yz::ToLower(kCandidates[i]);
        if (lowerName.find(needle) != std::wstring::npos)
            return true;
        if (!lowerPath.empty() && lowerPath.find(needle) != std::wstring::npos)
            return true;
    }
    return false;
}

void CollectServices(ProbeData& data)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (scm == nullptr)
    {
        data.notes.push_back(L"服务枚举失败（需要管理员权限）: " + yz::Win32ErrorMessage(GetLastError()));
        return;
    }

    DWORD needed = 0, returned = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER | SERVICE_WIN32,
                          SERVICE_STATE_ALL, nullptr, 0, &needed, &returned, &resume, nullptr);
    if (needed == 0)
    {
        CloseServiceHandle(scm);
        return;
    }

    std::vector<BYTE> buffer(needed);
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER | SERVICE_WIN32,
                               SERVICE_STATE_ALL, buffer.data(), needed, &needed, &returned,
                               &resume, nullptr))
    {
        CloseServiceHandle(scm);
        return;
    }

    ENUM_SERVICE_STATUS_PROCESSW* items = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD i = 0; i < returned; i++)
    {
        ProbeService s;
        s.name    = items[i].lpServiceName != nullptr ? items[i].lpServiceName : L"";
        s.display = items[i].lpDisplayName != nullptr ? items[i].lpDisplayName : L"";
        s.state   = items[i].ServiceStatusProcess.dwCurrentState;
        s.isDriver = (items[i].ServiceStatusProcess.dwServiceType & SERVICE_DRIVER) != 0;

        DWORD cfgSize = 0, startType = 0;
        SC_HANDLE svc = OpenServiceW(scm, s.name.c_str(), SERVICE_QUERY_CONFIG);
        if (svc != nullptr)
        {
            QueryServiceConfigW(svc, nullptr, 0, &cfgSize);
            if (cfgSize != 0)
            {
                std::vector<BYTE> cfgBuf(cfgSize);
                QUERY_SERVICE_CONFIGW* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfgBuf.data());
                if (QueryServiceConfigW(svc, cfg, cfgSize, &cfgSize))
                {
                    if (cfg->lpBinaryPathName != nullptr)
                        s.imagePath = cfg->lpBinaryPathName;
                    startType = cfg->dwStartType;
                }
            }
            CloseServiceHandle(svc);
        }
        s.startType = startType;
        s.isYuanzhi = LooksYuanzhiService(s.name, s.imagePath);

        if (s.isYuanzhi)
            data.services.push_back(s);
    }

    CloseServiceHandle(scm);
}

std::wstring IpToString(DWORD addr)
{
    return yz::Format(L"%u.%u.%u.%u",
                      addr & 0xFF, (addr >> 8) & 0xFF, (addr >> 16) & 0xFF, (addr >> 24) & 0xFF);
}

std::wstring PortToString(DWORD port)
{
    return yz::Format(L"%u", ((port & 0xFF) << 8) | ((port >> 8) & 0xFF));
}

void CollectNet(ProbeData& data)
{
    DWORD size = 0;
    if (GetExtendedUdpTable(nullptr, &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == ERROR_INSUFFICIENT_BUFFER)
    {
        std::vector<BYTE> buf(size);
        if (GetExtendedUdpTable(buf.data(), &size, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR)
        {
            MIB_UDPTABLE_OWNER_PID* table = reinterpret_cast<MIB_UDPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < table->dwNumEntries; i++)
            {
                ProbeNet n;
                n.pid      = table->table[i].dwOwningPid;
                n.protocol = L"UDP";
                n.local    = IpToString(table->table[i].dwLocalAddr) + L":" + PortToString(table->table[i].dwLocalPort);
                n.remote   = L"*";
                data.net.push_back(n);
            }
        }
    }

    size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER)
    {
        std::vector<BYTE> buf(size);
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR)
        {
            MIB_TCPTABLE_OWNER_PID* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buf.data());
            for (DWORD i = 0; i < table->dwNumEntries; i++)
            {
                ProbeNet n;
                n.pid      = table->table[i].dwOwningPid;
                n.protocol = (table->table[i].dwState == MIB_TCP_STATE_LISTEN) ? L"TCP-LISTEN" : L"TCP";
                n.local    = IpToString(table->table[i].dwLocalAddr) + L":" + PortToString(table->table[i].dwLocalPort);
                n.remote   = IpToString(table->table[i].dwRemoteAddr) + L":" + PortToString(table->table[i].dwRemotePort);
                data.net.push_back(n);
            }
        }
    }
}

bool HasPrivilege(HANDLE token, const wchar_t* name)
{
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, name, &luid))
        return false;

    DWORD size = 0;
    GetTokenInformation(token, TokenPrivileges, nullptr, 0, &size);
    if (size == 0)
        return false;
    std::vector<BYTE> buf(size);
    if (!GetTokenInformation(token, TokenPrivileges, buf.data(), size, &size))
        return false;

    TOKEN_PRIVILEGES* tp = reinterpret_cast<TOKEN_PRIVILEGES*>(buf.data());
    for (DWORD i = 0; i < tp->PrivilegeCount; i++)
    {
        if (tp->Privileges[i].Luid.LowPart == luid.LowPart &&
            tp->Privileges[i].Luid.HighPart == luid.HighPart)
            return true;
    }
    return false;
}

void CollectSelf(ProbeData& data)
{
    data.self.pid  = GetCurrentProcessId();
    data.self.path = yz::GetSelfPath();
    data.self.arch = SelfArchText();
    data.self.user = yz::GetHandleUser(GetCurrentProcess());
    data.self.integrity = yz::GetHandleIntegrityRid(GetCurrentProcess());
    yz::IsProcessElevated(&data.self.elevated);
    CollectModules(data.self.pid, data.self.modules);

    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        data.self.hasSeDebug = HasPrivilege(token, SE_DEBUG_NAME);
        CloseHandle(token);
    }
}

void CollectOsInfo(ProbeData& data)
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {0};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(name, &size))
        data.computerName = name;

    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0,
                      KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
    {
        wchar_t product[256] = {0};
        wchar_t build[256] = {0};
        wchar_t display[256] = {0};
        DWORD cb = sizeof(product);
        RegQueryValueExW(key, L"ProductName", nullptr, nullptr, reinterpret_cast<LPBYTE>(product), &cb);
        cb = sizeof(build);
        RegQueryValueExW(key, L"CurrentBuildNumber", nullptr, nullptr, reinterpret_cast<LPBYTE>(build), &cb);
        cb = sizeof(display);
        RegQueryValueExW(key, L"DisplayVersion", nullptr, nullptr, reinterpret_cast<LPBYTE>(display), &cb);
        data.osVersion = yz::Format(L"%s (Build %s, %s)", product, build, display);
        RegCloseKey(key);
    }
}
} /* namespace */

bool ProbeIsYuanzhiPath(const std::wstring& path, const std::wstring& exeName)
{
    if (!path.empty() &&
        (yz::ContainsNoCase(path, L"YZinfo Multimedia teaching software") ||
         yz::ContainsNoCase(path, L"GZYZ")))
        return true;

    static const wchar_t* kNames[] =
    {
        L"Yistart.exe", L"TEACHCMD.exe", L"PlayerGUI.exe", L"ExdPaintHelper.exe",
        L"Nmdeputy.exe", L"PMonitorNO.exe", L"ExdDrvGuard.exe", L"KillMain.exe",
        L"ScreenRecoder.exe", L"TEACHCMD.exe", L"Pointer.exe", L"ProxyC.exe", L"ProxyM.exe"
    };
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++)
    {
        if (_wcsicmp(exeName.c_str(), kNames[i]) == 0)
            return true;
    }
    return false;
}

bool ProbeCollect(ProbeData& data, bool includeAllModules)
{
    data.generatedAt = yz::NowStampEx();
    CollectOsInfo(data);
    CollectSelf(data);
    CollectProcesses(data, includeAllModules);
    CollectWindows(data);
    CollectServices(data);
    CollectNet(data);
    return true;
}


/* ---- PE 导出表解析（只读文件，不加载模块） ---- */
namespace
{
bool ReadWholeFile(const std::wstring& path, std::vector<BYTE>& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 128ll * 1024 * 1024)
    {
        CloseHandle(h);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) != FALSE;
    CloseHandle(h);
    return ok && read == out.size();
}

DWORD RvaToOffset(const std::vector<BYTE>& data, DWORD rva)
{
    if (data.size() < sizeof(IMAGE_DOS_HEADER))
        return 0;
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
        return 0;
    size_t ntOff = static_cast<size_t>(dos->e_lfanew);
    if (ntOff + sizeof(IMAGE_NT_HEADERS32) > data.size())
        return 0;
    const IMAGE_NT_HEADERS32* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(data.data() + ntOff);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        DWORD va = sec[i].VirtualAddress;
        DWORD vsize = sec[i].Misc.VirtualSize;
        if (vsize == 0)
            vsize = sec[i].SizeOfRawData;
        if (rva >= va && rva < va + vsize)
            return sec[i].PointerToRawData + (rva - va);
    }
    return 0;
}
} /* namespace */

bool ProbeReadExports(const std::wstring& filePath, std::vector<std::wstring>& out)
{
    std::vector<BYTE> data;
    if (!ReadWholeFile(filePath, data))
        return false;

    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(data.data());
    size_t ntOff = static_cast<size_t>(dos->e_lfanew);
    const IMAGE_NT_HEADERS32* nt32 = reinterpret_cast<const IMAGE_NT_HEADERS32*>(data.data() + ntOff);
    bool is64 = nt32->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 ||
                nt32->FileHeader.Machine == IMAGE_FILE_MACHINE_ARM64;

    DWORD exportRva = 0;
    if (is64)
    {
        const IMAGE_NT_HEADERS64* nt64 = reinterpret_cast<const IMAGE_NT_HEADERS64*>(data.data() + ntOff);
        exportRva = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    }
    else
    {
        exportRva = nt32->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    }
    if (exportRva == 0)
        return false;

    DWORD expOff = RvaToOffset(data, exportRva);
    if (expOff == 0 || expOff + sizeof(IMAGE_EXPORT_DIRECTORY) > data.size())
        return false;

    const IMAGE_EXPORT_DIRECTORY* exp =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(data.data() + expOff);
    if (exp->NumberOfNames == 0 || exp->AddressOfNames == 0)
        return false;

    DWORD namesOff = RvaToOffset(data, exp->AddressOfNames);
    if (namesOff == 0)
        return false;

    const DWORD* names = reinterpret_cast<const DWORD*>(data.data() + namesOff);
    for (DWORD i = 0; i < exp->NumberOfNames && out.size() < 512; i++)
    {
        DWORD nameOff = RvaToOffset(data, names[i]);
        if (nameOff == 0 || nameOff >= data.size())
            continue;
        const char* name = reinterpret_cast<const char*>(data.data() + nameOff);
        size_t maxLen = data.size() - nameOff;
        size_t len = strnlen_s(name, maxLen);
        if (len == 0 || len == maxLen)
            continue;
        out.push_back(yz::Utf8ToWide(std::string(name, len)));
    }
    return !out.empty();
}
