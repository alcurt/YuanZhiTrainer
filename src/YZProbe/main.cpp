//
// YZProbe.exe —— 机房只读侦察工具。
//
// 用法：
//   YZProbe.exe [-o 输出目录] [--modules]
//     -o        指定报告输出目录（默认 exe 所在目录）
//     --modules 列出所有进程的模块（默认只列远志相关进程与本进程）
//
#include "probe.h"

#include "../common/yz_util.h"
#include "yz_protocol.h"

#include <stdio.h>
#include <locale.h>

namespace
{
std::wstring JsonEscape(const std::wstring& s)
{
    std::wstring out;
    for (size_t i = 0; i < s.size(); i++)
    {
        wchar_t c = s[i];
        switch (c)
        {
        case L'\\': out += L"\\\\"; break;
        case L'"':  out += L"\\\""; break;
        case L'\r': out += L"\\r"; break;
        case L'\n': out += L"\\n"; break;
        case L'\t': out += L"\\t"; break;
        default:
            if (c < 0x20)
                out += yz::Format(L"\\u%04X", static_cast<unsigned>(c));
            else
                out += c;
            break;
        }
    }
    return out;
}

std::wstring IntegrityName(DWORD rid)
{
    if (rid == 0)      return L"未知";
    if (rid < 8192)    return yz::Format(L"低(%u)", rid);
    if (rid < 12288)   return yz::Format(L"中(%u)", rid);
    if (rid < 16384)   return yz::Format(L"高(%u)", rid);
    return yz::Format(L"系统(%u)", rid);
}

std::wstring StateName(DWORD state)
{
    switch (state)
    {
    case SERVICE_RUNNING: return L"运行中";
    case SERVICE_STOPPED: return L"已停止";
    case SERVICE_START_PENDING: return L"启动中";
    case SERVICE_STOP_PENDING: return L"停止中";
    default: return yz::Format(L"%u", state);
    }
}

/* 进程保护级别文本（PROTECTION_LEVEL 枚举，见 WinBase.h） */
std::wstring ProtectionText(DWORD level)
{
    switch (level)
    {
    case 0xFFFFFFFE: return L"无";
    case 0x00000000: return L"PPL/WinTcbLight";
    case 0x00000001: return L"PP/Windows";
    case 0x00000002: return L"PPL/WindowsLight";
    case 0x00000003: return L"PPL/AntimalwareLight";
    case 0x00000004: return L"PPL/LsaLight";
    case 0x00000005: return L"PP/WinTcb";
    case 0x00000006: return L"PPL/CodeGenLight";
    case 0x00000007: return L"PP/Authenticode";
    case 0x00000008: return L"PPL_APP";
    default:         return yz::Format(L"未知(0x%08X)", level);
    }
}

void WriteUtf8File(const std::wstring& path, const std::wstring& text)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    std::string utf8 = yz::WideToUtf8(text);
    /* 带 UTF-8 BOM，方便记事本/VS Code 直接识别 */
    const char bom[3] = { '\xEF', '\xBB', '\xBF' };
    DWORD written = 0;
    WriteFile(h, bom, 3, &written, nullptr);
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
}
} /* namespace */

std::wstring ProbeToJson(const ProbeData& data)
{
    std::wstring j;
    j += L"{\n";
    j += L"  \"tool\": \"YZProbe\",\n";
    j += yz::Format(L"  \"version\": \"%s\",\n", YZ_VERSION_STR);
    j += L"  \"generatedAt\": \"" + JsonEscape(data.generatedAt) + L"\",\n";
    j += L"  \"computer\": \"" + JsonEscape(data.computerName) + L"\",\n";
    j += L"  \"os\": \"" + JsonEscape(data.osVersion) + L"\",\n";

    j += L"  \"self\": {\n";
    j += yz::Format(L"    \"pid\": %u,\n", data.self.pid);
    j += L"    \"path\": \"" + JsonEscape(data.self.path) + L"\",\n";
    j += L"    \"arch\": \"" + JsonEscape(data.self.arch) + L"\",\n";
    j += L"    \"user\": \"" + JsonEscape(data.self.user) + L"\",\n";
    j += L"    \"integrity\": \"" + JsonEscape(IntegrityName(data.self.integrity)) + L"\",\n";
    j += yz::Format(L"    \"elevated\": %s,\n", data.self.elevated ? L"true" : L"false");
    j += yz::Format(L"    \"hasSeDebugPrivilege\": %s,\n", data.self.hasSeDebug ? L"true" : L"false");
    j += L"    \"modules\": [";
    for (size_t i = 0; i < data.self.modules.size(); i++)
    {
        if (i != 0) j += L", ";
        j += L"\"" + JsonEscape(data.self.modules[i].name) + L"\"";
    }
    j += L"]\n  },\n";

    j += L"  \"processes\": [\n";
    for (size_t i = 0; i < data.processes.size(); i++)
    {
        const ProbeProcess& p = data.processes[i];
        j += L"    {\n";
        j += yz::Format(L"      \"pid\": %u,\n", p.pid);
        j += L"      \"name\": \"" + JsonEscape(p.name) + L"\",\n";
        j += L"      \"path\": \"" + JsonEscape(p.path) + L"\",\n";
        j += L"      \"user\": \"" + JsonEscape(p.user) + L"\",\n";
        j += L"      \"integrity\": \"" + JsonEscape(IntegrityName(p.integrity)) + L"\",\n";
        j += yz::Format(L"      \"elevated\": %s,\n", p.elevated ? L"true" : L"false");
        j += yz::Format(L"      \"isYuanzhi\": %s,\n", p.isYuanzhi ? L"true" : L"false");
        j += yz::Format(L"      \"protectionLevel\": \"0x%08X\",\n", p.protectionLevel);
        j += L"      \"modules\": [";
        for (size_t k = 0; k < p.modules.size(); k++)
        {
            if (k != 0) j += L", ";
            j += L"\"" + JsonEscape(p.modules[k].name) + L"\"";
        }
        j += L"],\n      \"moduleExports\": {";
        bool firstMod = true;
        for (size_t k = 0; k < p.modules.size(); k++)
        {
            if (p.modules[k].exports.empty())
                continue;
            if (!firstMod)
                j += L", ";
            firstMod = false;
            j += L"\"" + JsonEscape(p.modules[k].name) + L"\": [";
            for (size_t e = 0; e < p.modules[k].exports.size(); e++)
            {
                if (e != 0)
                    j += L", ";
                j += L"\"" + JsonEscape(p.modules[k].exports[e]) + L"\"";
            }
            j += L"]";
        }
        j += L"},\n      \"diskModuleExports\": {";
        bool firstDisk = true;
        for (size_t k = 0; k < p.diskModules.size(); k++)
        {
            if (!firstDisk)
                j += L", ";
            firstDisk = false;
            j += L"\"" + JsonEscape(p.diskModules[k].name) + L"\": [";
            for (size_t e = 0; e < p.diskModules[k].exports.size(); e++)
            {
                if (e != 0)
                    j += L", ";
                j += L"\"" + JsonEscape(p.diskModules[k].exports[e]) + L"\"";
            }
            j += L"]";
        }
        j += L"}\n    }";
        if (i + 1 != data.processes.size()) j += L",";
        j += L"\n";
    }
    j += L"  ],\n";

    j += L"  \"access\": [\n";
    for (size_t i = 0; i < data.access.size(); i++)
    {
        const ProbeAccess& a = data.access[i];
        j += L"    {\n";
        j += yz::Format(L"      \"pid\": %u,\n", a.pid);
        j += yz::Format(L"      \"protectionLevel\": \"0x%08X\",\n", a.protectionLevel);
        j += L"      \"protection\": \"" + JsonEscape(a.protectionText) + L"\",\n";
        j += (a.grantedAccess != 0)
                 ? yz::Format(L"      \"grantedAccess\": \"0x%08X\",\n", static_cast<unsigned>(a.grantedAccess))
                 : std::wstring(L"      \"grantedAccess\": \"未取到\",\n");
        j += L"      \"moduleRead\": \"" + JsonEscape(a.moduleRead) + L"\",\n";
        j += L"      \"vmRead\": \"" + JsonEscape(a.vmRead) + L"\",\n";
        j += L"      \"vmOperation\": \"" + JsonEscape(a.vmOperation) + L"\",\n";
        j += L"      \"vmWrite\": \"" + JsonEscape(a.vmWrite) + L"\",\n";
        j += L"      \"createThread\": \"" + JsonEscape(a.createThread) + L"\"\n";
        j += L",\n      \"privNote\": \"" + JsonEscape(a.privNote) + L"\"\n";
        j += L"    }";
        if (i + 1 != data.access.size()) j += L",";
        j += L"\n";
    }
    j += L"  ],\n";

    j += L"  \"windows\": [\n";
    for (size_t i = 0; i < data.windows.size(); i++)
    {
        const ProbeWindow& w = data.windows[i];
        j += L"    {\n";
        j += yz::Format(L"      \"hwnd\": \"0x%p\",\n", w.hwnd);
        j += yz::Format(L"      \"pid\": %u,\n", w.pid);
        j += L"      \"process\": \"" + JsonEscape(w.processName) + L"\",\n";
        j += L"      \"class\": \"" + JsonEscape(w.className) + L"\",\n";
        j += L"      \"title\": \"" + JsonEscape(w.title) + L"\",\n";
        j += yz::Format(L"      \"rect\": [%d, %d, %d, %d],\n", w.rect.left, w.rect.top, w.rect.right, w.rect.bottom);
        j += yz::Format(L"      \"style\": \"0x%08X\",\n", static_cast<unsigned>(w.style));
        j += yz::Format(L"      \"exStyle\": \"0x%08X\",\n", static_cast<unsigned>(w.exStyle));
        j += yz::Format(L"      \"topmost\": %s,\n", w.topmost ? L"true" : L"false");
        j += yz::Format(L"      \"visible\": %s,\n", w.visible ? L"true" : L"false");
        j += yz::Format(L"      \"borderless\": %s,\n", w.borderless ? L"true" : L"false");
        j += yz::Format(L"      \"coversMonitor\": %s,\n", w.coversMonitor ? L"true" : L"false");
        j += yz::Format(L"      \"isYuanzhi\": %s\n", w.isYuanzhi ? L"true" : L"false");
        j += L"    }";
        if (i + 1 != data.windows.size()) j += L",";
        j += L"\n";
    }
    j += L"  ],\n";

    j += L"  \"services\": [\n";
    for (size_t i = 0; i < data.services.size(); i++)
    {
        const ProbeService& s = data.services[i];
        j += L"    {";
        j += L"\"name\": \"" + JsonEscape(s.name) + L"\", ";
        j += L"\"display\": \"" + JsonEscape(s.display) + L"\", ";
        j += L"\"imagePath\": \"" + JsonEscape(s.imagePath) + L"\", ";
        j += L"\"state\": \"" + JsonEscape(StateName(s.state)) + L"\", ";
        j += yz::Format(L"\"isDriver\": %s, ", s.isDriver ? L"true" : L"false");
        j += yz::Format(L"\"launchProtected\": %u}", s.launchProtected);
        if (i + 1 != data.services.size()) j += L",";
        j += L"\n";
    }
    j += L"  ],\n";

    j += L"  \"thirdPartyDrivers\": [\n";
    for (size_t i = 0; i < data.drivers.size(); i++)
    {
        const ProbeDriver& d = data.drivers[i];
        j += L"    {\"name\": \"" + JsonEscape(d.name) + L"\", ";
        j += L"\"company\": \"" + JsonEscape(d.company) + L"\", ";
        j += L"\"fileVersion\": \"" + JsonEscape(d.fileVersion) + L"\", ";
        j += L"\"imagePath\": \"" + JsonEscape(d.imagePath) + L"\", ";
        j += yz::Format(L"\"state\": %u}", d.state);
        if (i + 1 != data.drivers.size()) j += L",";
        j += L"\n";
    }
    j += L"  ],\n";

    j += L"  \"net\": [\n";
    for (size_t i = 0; i < data.net.size(); i++)
    {
        const ProbeNet& n = data.net[i];
        j += yz::Format(L"    {\"pid\": %u, \"proto\": \"%s\", \"local\": \"%s\", \"remote\": \"%s\"}",
                        n.pid, n.protocol.c_str(), n.local.c_str(), n.remote.c_str());
        if (i + 1 != data.net.size()) j += L",";
        j += L"\n";
    }
    j += L"  ],\n";

    j += L"  \"notes\": [";
    for (size_t i = 0; i < data.notes.size(); i++)
    {
        if (i != 0) j += L", ";
        j += L"\"" + JsonEscape(data.notes[i]) + L"\"";
    }
    j += L"]\n}\n";
    return j;
}

std::wstring ProbeToMarkdown(const ProbeData& data)
{
    std::wstring m;
    m += L"# YZProbe 侦察报告\n\n";
    m += L"- 生成时间: " + data.generatedAt + L"\n";
    m += L"- 计算机: " + data.computerName + L"\n";
    m += L"- 系统: " + data.osVersion + L"\n";
    m += yz::Format(L"- 探针: PID %u, %s, %s, 完整性=%s, 提权=%s, SeDebug=%s\n\n",
                    data.self.pid, data.self.arch.c_str(), data.self.user.c_str(),
                    IntegrityName(data.self.integrity).c_str(),
                    data.self.elevated ? L"是" : L"否",
                    data.self.hasSeDebug ? L"有" : L"无");

    m += L"## 远志相关进程\n\n";
    m += L"| PID | 进程 | 用户 | 完整性 | 提权 | 进程保护 | 路径 |\n|---|---|---|---|---|---|---|\n";
    for (size_t i = 0; i < data.processes.size(); i++)
    {
        const ProbeProcess& p = data.processes[i];
        if (!p.isYuanzhi)
            continue;
        m += yz::Format(L"| %u | %s | %s | %s | %s | %s | %s |\n", p.pid, p.name.c_str(), p.user.c_str(),
                        IntegrityName(p.integrity).c_str(), p.elevated ? L"是" : L"否",
                        ProtectionText(p.protectionLevel).c_str(), p.path.c_str());
    }

    m += L"\n### 远志进程加载的关键模块\n\n";
    for (size_t i = 0; i < data.processes.size(); i++)
    {
        const ProbeProcess& p = data.processes[i];
        if (!p.isYuanzhi || p.modules.empty())
            continue;
        m += yz::Format(L"**PID %u (%s)**\n\n```\n", p.pid, p.name.c_str());
        for (size_t k = 0; k < p.modules.size(); k++)
        {
            const std::wstring& mod = p.modules[k].name;
            if (yz::ContainsNoCase(mod, L".ads") || yz::ContainsNoCase(mod, L"Exd") ||
                yz::ContainsNoCase(mod, L"Player") || yz::ContainsNoCase(mod, L"Hook") ||
                yz::ContainsNoCase(mod, L"Mfc") || yz::ContainsNoCase(mod, L"Rtp") ||
                yz::ContainsNoCase(mod, L"Udp") || yz::ContainsNoCase(mod, L"Svf"))
            {
                m += L"  " + mod + L"\n";
            }
        }
        m += L"```\n\n";
    }

    m += L"### 远志模块导出表（可用于确认可直接调用的入口）\n\n";
    for (size_t i = 0; i < data.processes.size(); i++)
    {
        const ProbeProcess& p = data.processes[i];
        if (!p.isYuanzhi)
            continue;
        for (size_t k = 0; k < p.modules.size(); k++)
        {
            if (p.modules[k].exports.empty())
                continue;
            m += L"**" + p.modules[k].name + L"**\n\n```\n";
            for (size_t e = 0; e < p.modules[k].exports.size(); e++)
            {
                m += p.modules[k].exports[e];
                m += (e + 1 == p.modules[k].exports.size()) ? L"\n" : L", ";
            }
            m += L"```\n\n";
        }
    }

    m += L"### 安装目录关键模块的导出表（进程内模块读不到时的兜底）\n\n";
    bool anyDisk = false;
    for (size_t i = 0; i < data.processes.size(); i++)
    {
        const ProbeProcess& p = data.processes[i];
        if (!p.isYuanzhi || p.diskModules.empty())
            continue;
        anyDisk = true;
        m += yz::Format(L"**PID %u (%s) 的安装目录**\n\n", p.pid, p.name.c_str());
        for (size_t k = 0; k < p.diskModules.size(); k++)
        {
            m += L"`" + p.diskModules[k].name + L"`：";
            for (size_t e = 0; e < p.diskModules[k].exports.size(); e++)
            {
                m += p.diskModules[k].exports[e];
                m += (e + 1 == p.diskModules[k].exports.size()) ? L"\n" : L", ";
            }
        }
        m += L"\n";
    }
    if (!anyDisk)
        m += L"（未采集：进程模块可正常读取，或没有远志进程）\n\n";

    m += L"## 注入能力实测（回答“为什么注不进去”）\n\n";
    if (data.access.empty())
    {
        m += L"（未采集：非管理员运行，或这次没有远志进程）\n\n";
    }
    else
    {
        m += L"| PID | 进程保护 | 句柄实得权限 | 模块枚举 | ReadProcessMemory | VirtualAllocEx | WriteProcessMemory |\n";
        m += L"|---|---|---|---|---|---|---|\n";
        for (size_t i = 0; i < data.access.size(); i++)
        {
            const ProbeAccess& a = data.access[i];
            const std::wstring granted = (a.grantedAccess != 0)
                ? yz::Format(L"0x%08X", static_cast<unsigned>(a.grantedAccess))
                : std::wstring(L"未取到");
            m += yz::Format(L"| %u | %s | %s | %s | %s | %s | %s |\n",
                            a.pid, a.protectionText.c_str(), granted.c_str(),
                            a.moduleRead.c_str(), a.vmRead.c_str(), a.vmOperation.c_str(),
                            a.vmWrite.c_str());
        }
        for (size_t i = 0; i < data.access.size(); i++)
        {
            if (!data.access[i].privNote.empty())
            {
                m += yz::Format(L"- PID %u：%s\n", data.access[i].pid,
                                data.access[i].privNote.c_str());
            }
        }
        m += L"\n权限位对照：`PROCESS_VM_OPERATION=0x0008`、`PROCESS_VM_WRITE=0x0020`、"
             L"`PROCESS_VM_READ=0x0010`、`PROCESS_CREATE_THREAD=0x0002`。"
             L"请求了这些位而“实得权限”里没有，就说明被内核组件（句柄权限剥夺）或进程保护机制拿掉了——"
             L"这正是“OpenProcess 成功、VirtualAllocEx 返回 0x5”的原因。\n\n";
    }

    m += L"## 疑似全屏/置顶窗口（按进程排序）\n\n";
    m += L"| HWND | PID | 进程 | 类名 | 样式 | 置顶 | 无边框 | 覆盖整屏 | 可见 | 标题 |\n|---|---|---|---|---|---|---|---|---|---|\n";
    for (size_t i = 0; i < data.windows.size(); i++)
    {
        const ProbeWindow& w = data.windows[i];
        if (!w.coversMonitor && !w.isYuanzhi)
            continue;
        m += yz::Format(L"| %p | %u | %s | %s | 0x%08X/0x%08X | %s | %s | %s | %s | %s |\n",
                        w.hwnd, w.pid, w.processName.c_str(), w.className.c_str(),
                        static_cast<unsigned>(w.style), static_cast<unsigned>(w.exStyle),
                        w.topmost ? L"是" : L"否", w.borderless ? L"是" : L"否",
                        w.coversMonitor ? L"是" : L"否", w.visible ? L"是" : L"否",
                        w.title.c_str());
    }

    m += L"\n## 远志相关服务 / 驱动\n\n";
    m += L"| 名称 | 显示名 | 状态 | 驱动 | 受保护启动 | ImagePath |\n|---|---|---|---|---|---|\n";
    for (size_t i = 0; i < data.services.size(); i++)
    {
        const ProbeService& s = data.services[i];
        m += yz::Format(L"| %s | %s | %s | %s | %u | %s |\n", s.name.c_str(), s.display.c_str(),
                        StateName(s.state).c_str(), s.isDriver ? L"是" : L"否",
                        s.launchProtected, s.imagePath.c_str());
    }

    m += L"\n## 非微软签名的内核驱动（保护件/杀软/还原卡通常在这张表里）\n\n";
    if (data.drivers.empty())
    {
        m += L"（无，或没有权限枚举）\n\n";
    }
    else
    {
    m += L"| 服务名 | 状态 | 厂商 | 文件版本 | ImagePath |\n|---|---|---|---|---|\n";
    for (size_t i = 0; i < data.drivers.size(); i++)
    {
        const ProbeDriver& d = data.drivers[i];
        m += yz::Format(L"| %s | %u | %s | %s | %s |\n", d.name.c_str(), d.state,
                        d.company.empty() ? L"(无版本信息)" : d.company.c_str(),
                        d.fileVersion.empty() ? L"-" : d.fileVersion.c_str(),
                        d.imagePath.c_str());
    }
        m += L"\n";
    }

    m += L"\n## 远志进程的网络端点\n\n";
    m += L"| PID | 协议 | 本地 | 远端 |\n|---|---|---|---|\n";
    for (size_t i = 0; i < data.net.size(); i++)
    {
        const ProbeNet& n = data.net[i];
        bool yuanzhiPid = false;
        for (size_t k = 0; k < data.processes.size(); k++)
        {
            if (data.processes[k].pid == n.pid && data.processes[k].isYuanzhi)
            {
                yuanzhiPid = true;
                break;
            }
        }
        if (!yuanzhiPid)
            continue;
        m += yz::Format(L"| %u | %s | %s | %s |\n", n.pid, n.protocol.c_str(), n.local.c_str(), n.remote.c_str());
    }

    if (!data.notes.empty())
    {
        m += L"\n## 备注\n\n";
        for (size_t i = 0; i < data.notes.size(); i++)
            m += L"- " + data.notes[i] + L"\n";
    }
    return m;
}

int wmain(int argc, wchar_t** argv)
{
    /* 控制台按 UTF-8 输出，CRT 也要用 UTF-8 区域设置，否则宽字符会退化成 '?' */
    SetConsoleOutputCP(CP_UTF8);
    setlocale(LC_ALL, ".UTF8");

    std::wstring outDir = yz::GetExeDir();
    bool includeAllModules = false;
    DWORD accessPid = 0;
    bool  noPause = false;

    for (int i = 1; i < argc; i++)
    {
        std::wstring arg = argv[i];
        if ((arg == L"-o" || arg == L"--out") && i + 1 < argc)
        {
            outDir = argv[++i];
        }
        else if (arg == L"--modules")
        {
            includeAllModules = true;
        }
        else if (arg == L"--access" && i + 1 < argc)
        {
            accessPid = static_cast<DWORD>(_wtoi(argv[++i]));
        }
        else if (arg == L"--no-pause")
        {
            noPause = true;
        }
        else if (arg == L"-h" || arg == L"--help")
        {
            wprintf(L"用法: YZProbe.exe [-o 输出目录] [--modules] [--access <pid>] [--no-pause]\n");
            wprintf(L"  --modules       采集所有进程的模块（默认只采集远志相关进程）\n");
            wprintf(L"  --access <pid>  对指定进程做注入能力探测（保护级别/句柄权限/读/分配/写）\n");
            wprintf(L"  双击运行时结束会等你按回车；脚本里用 --no-pause 跳过\n");
            if (!noPause)
                yz::PauseIfSoleConsole();
            return 0;
        }
    }

    wprintf(L"YZProbe 正在采集（管理员权限下信息最完整）...\n");

    ProbeData data;
    ProbeCollect(data, includeAllModules);
    if (accessPid != 0)
    {
        ProbeAccessProbePid(accessPid, data);
        data.notes.push_back(yz::Format(L"按 --access 参数额外探测了 PID=%u", accessPid));
    }

    yz::EnsureDirectory(outDir);
    std::wstring stamp = yz::NowStamp();
    std::wstring jsonPath = yz::JoinPath(outDir, L"YZProbe-report-" + stamp + L".json");
    std::wstring mdPath   = yz::JoinPath(outDir, L"YZProbe-report-" + stamp + L".md");

    WriteUtf8File(jsonPath, ProbeToJson(data));
    WriteUtf8File(mdPath, ProbeToMarkdown(data));

    size_t yuanzhiProcesses = 0;
    for (size_t i = 0; i < data.processes.size(); i++)
    {
        if (data.processes[i].isYuanzhi)
            yuanzhiProcesses++;
    }

    wprintf(L"采集完成：\n");
    wprintf(L"  进程 %zu 个（其中远志相关 %zu 个）\n", data.processes.size(), yuanzhiProcesses);
    wprintf(L"  窗口 %zu 个，服务/驱动 %zu 个，网络端点 %zu 条\n",
            data.windows.size(), data.services.size(), data.net.size());
    wprintf(L"  非微软内核驱动 %zu 个，注入能力探测 %zu 条\n",
            data.drivers.size(), data.access.size());
    wprintf(L"  JSON: %ls\n", jsonPath.c_str());
    wprintf(L"  Markdown: %ls\n", mdPath.c_str());
    if (!noPause)
        yz::PauseIfSoleConsole();
    return 0;
}

