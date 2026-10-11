//
// YZProbe.exe —— 机房只读侦察工具。
//
// 用法：
//   YZProbe.exe [-o 输出目录] [--modules]
//     -o        指定报告输出目录（默认 exe 所在目录）
//     --modules 列出所有进程的模块（默认只列远志相关进程与本进程）
//
#include "probe.h"

#include "../common/yz_capture.h"
#include "../common/yz_util.h"
#include "yz_protocol.h"

#include <stdio.h>
#include <locale.h>
#include <map>
#include <set>

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

/* 进程保护级别文本（PROTECTION_LEVEL 枚举，见 WinBase.h）。
   注意 level==0 是**合法**的 PROTECTION_LEVEL_WINTCB_LIGHT，不能拿它当"查不到"的哨兵：
   查询失败必须走 ProtectionTextFor(known=false) 那条路（v0.6.23 修的就是把二者混起来，
   结果现场最想看的那个抓屏进程被报成"受 PPL 保护"）。 */
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

/* known=false 表示查询失败（句柄打不开或 API 不可用）：只说"未知"，并保留一句有信息量的提示，
   不去猜具体级别。level==0 是合法的 WINTCB_LIGHT，所以绝不能用它当"查不到"的哨兵。 */
std::wstring ProtectionTextFor(bool known, DWORD level)
{
    if (known)
        return ProtectionText(level);
    return L"未知（查询失败：句柄打不开，常见于 PPL/受保护进程）";
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
        j += yz::Format(L"      \"protectionKnown\": %s,\n", p.protectionKnown ? L"true" : L"false");
        j += yz::Format(L"      \"modulesReadable\": %s,\n", p.modulesReadable ? L"true" : L"false");
        j += yz::Format(L"      \"hasCaptureModule\": %s,\n", p.hasCaptureModule ? L"true" : L"false");
        j += L"      \"captureModule\": \"" + JsonEscape(p.captureModuleName) + L"\",\n";
        j += L"      \"captureModulePath\": \"" + JsonEscape(p.captureModulePath) + L"\",\n";
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
        j += L"      \"createThread\": \"" + JsonEscape(a.createThread) + L"\",\n";
        j += yz::Format(L"      \"grantedStatus\": \"0x%08X\",\n", static_cast<unsigned>(a.grantedStatus));
        j += L"      \"threadProbe\": \"" + JsonEscape(a.threadProbe) + L"\",\n";
        j += L"      \"privNote\": \"" + JsonEscape(a.privNote) + L"\",\n";
        j += L"      \"rights\": [";
        for (size_t r = 0; r < a.rights.size(); r++)
        {
            const ProbeAccessRight& rt = a.rights[r];
            j += L"\n        {\n";
            j += L"          \"name\": \"" + JsonEscape(rt.name) + L"\",\n";
            j += yz::Format(L"          \"requested\": \"0x%08X\",\n", static_cast<unsigned>(rt.requested));
            j += yz::Format(L"          \"opened\": %s,\n", rt.opened ? L"true" : L"false");
            j += yz::Format(L"          \"err\": \"0x%08X\",\n", static_cast<unsigned>(rt.err));
            j += yz::Format(L"          \"granted\": \"0x%08X\",\n", static_cast<unsigned>(rt.granted));
            j += yz::Format(L"          \"queryStatus\": \"0x%08X\",\n", static_cast<unsigned>(rt.queryStatus));
            j += yz::Format(L"          \"stripped\": %s\n", rt.stripped ? L"true" : L"false");
            j += (r + 1 == a.rights.size()) ? L"        }\n" : L"        },\n";
        }
        j += L"      ]\n";
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
                        ProtectionTextFor(p.protectionKnown, p.protectionLevel).c_str(), p.path.c_str());
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
        for (size_t i = 0; i < data.access.size(); i++)
        {
            const ProbeAccess& a = data.access[i];
            if (a.rights.empty())
                continue;

            m += yz::Format(L"\n#### PID %u 的逐权限位实测\n\n", a.pid);
            m += L"| 请求权限 | OpenProcess | 句柄实得权限 | NtQueryObject | 判定 |\n";
            m += L"|---|---|---|---|---|\n";
            for (size_t r = 0; r < a.rights.size(); r++)
            {
                const ProbeAccessRight& rt = a.rights[r];
                const std::wstring openText = rt.opened
                    ? std::wstring(L"成功")
                    : yz::Format(L"失败 err=0x%08X", static_cast<unsigned>(rt.err));
                const std::wstring grantedText = rt.opened
                    ? yz::Format(L"0x%08X", static_cast<unsigned>(rt.granted))
                    : std::wstring(L"-");
                std::wstring verdict;
                if (!rt.opened)
                    verdict = L"打不开（无权限）";
                else if (rt.stripped)
                    verdict = L"**被削**（打开成功，但请求的位不在实得权限里）";
                else
                    verdict = L"完整";
                m += yz::Format(L"| %s (0x%08X) | %s | %s | 0x%08X | %s |\n",
                                rt.name.c_str(), static_cast<unsigned>(rt.requested),
                                openText.c_str(), grantedText.c_str(),
                                static_cast<unsigned>(rt.queryStatus), verdict.c_str());
            }
            if (!a.threadProbe.empty())
                m += yz::Format(L"\n- 目标线程句柄（`OpenThread`+`NtQueryObject`）：%s\n", a.threadProbe.c_str());
        }
        m += L"\n怎么读这张表：\n\n";
        m += L"* `打不开` = DACL/完整性级别不允许，属于“权限不够”，提权或换身份有效；\n";
        m += L"* `被削` = `OpenProcess` 成功但句柄里没有请求的位，只有内核组件"
             L"（`ObRegisterCallbacks` 一类）能在**不改 DACL、不受 `SeDebugPrivilege` 影响**的前提下做到——"
             L"这正是“`OpenProcess` 成功、`VirtualAllocEx` 返回 0x5”的机制；\n";
        m += L"* 权限位对照：`PROCESS_QUERY_LIMITED_INFORMATION=0x1000`、`PROCESS_QUERY_INFORMATION=0x0400`、"
             L"`PROCESS_VM_READ=0x0010`、`PROCESS_VM_WRITE=0x0020`、`PROCESS_VM_OPERATION=0x0008`、"
             L"`PROCESS_CREATE_THREAD=0x0002`。\n";
        m += L"* `NtQueryObject` 一列应当是 `0x00000000`；若这里是非零 NTSTATUS（例如 `0xC0000004` "
             L"= 缓冲区长度不符），说明权限位没读出来，那一行的“实得权限”不可信。\n\n";
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

/* ===================== --watch：触发式常驻采集 =====================
   动机（2026-10-10 教训）：教师监视/点名/全屏广播都是"一次、没预告、几分钟"，
   定时探针赌的是时间点，10-09 那次正好卡在广播开始前 3 秒，整段全屏态丢掉。
   这里改成"事件触发"：只要出现新远志进程 / 任何进程加载了采集组件 /
   出现疑似广播窗口，就立刻落一份完整报告，并往 watch 日志写一行。
   另每分钟写一条心跳——"没采到"和"没在跑"必须能区分开，否则数据无法判读。 */
namespace
{
std::wstring  g_watchLogPath;
bool          g_watchLogStarted = false;
volatile LONG g_watchStop       = 0;

BOOL WINAPI WatchConsoleHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
    {
        InterlockedExchange(&g_watchStop, 1);
        return TRUE;
    }
    return FALSE;
}

void WatchLogRaw(const std::wstring& text)
{
    HANDLE h = CreateFileW(g_watchLogPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    if (!g_watchLogStarted)
    {
        g_watchLogStarted = true;
        const char bom[3] = { '\xEF', '\xBB', '\xBF' };
        WriteFile(h, bom, 3, &written, nullptr);
        const std::wstring head = L"# YZProbe --watch 触发式采集日志（探针 " YZ_VERSION_STR L"）\r\n";
        const std::string headUtf8 = yz::WideToUtf8(head);
        WriteFile(h, headUtf8.data(), static_cast<DWORD>(headUtf8.size()), &written, nullptr);
    }
    const std::string utf8 = yz::WideToUtf8(text);
    WriteFile(h, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(h);
}

void WatchLog(const std::wstring& text)
{
    const std::wstring line = L"[" + yz::NowStampEx() + L"] " + text + L"\r\n";
    WatchLogRaw(line);
    wprintf(L"%s", line.c_str());
    fflush(stdout);
}

/* 报告落盘（wmain 与 --watch 共用） */
bool WriteReports(const std::wstring& outDir, const ProbeData& data,
                  const std::wstring& nameMid,
                  std::wstring* outJson, std::wstring* outMd)
{
    yz::EnsureDirectory(outDir);
    const std::wstring jsonPath = yz::JoinPath(outDir, L"YZProbe-report-" + nameMid + L".json");
    const std::wstring mdPath   = yz::JoinPath(outDir, L"YZProbe-report-" + nameMid + L".md");
    WriteUtf8File(jsonPath, ProbeToJson(data));
    WriteUtf8File(mdPath, ProbeToMarkdown(data));
    if (outJson != nullptr) *outJson = jsonPath;
    if (outMd != nullptr)   *outMd = mdPath;
    return yz::FileExists(jsonPath) && yz::FileExists(mdPath);
}

struct WatchWindowHit
{
    HWND         hwnd;
    DWORD        pid;
    std::wstring cls;
    std::wstring title;
    std::wstring exePath;
    LONG         style;
    LONG         exStyle;
    RECT         rect;
    std::wstring why;
};

BOOL CALLBACK WatchWindowProc(HWND hwnd, LPARAM lParam)
{
    std::vector<WatchWindowHit>* out = reinterpret_cast<std::vector<WatchWindowHit>*>(lParam);
    if (!IsWindowVisible(hwnd))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const std::wstring exePath = yz::GetProcessImagePath(pid);

    wchar_t cls[256] = {0};
    GetClassNameW(hwnd, cls, 256);
    const bool brwClass = _wcsnicmp(cls, L"Brw:", 4) == 0;
    const bool vendor   = yz::IsYuanzhiInstallPath(exePath);
    if (!brwClass && !vendor)
        return TRUE;

    RECT rc;
    GetWindowRect(hwnd, &rc);
    const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
    const LONG ex    = GetWindowLongW(hwnd, GWL_EXSTYLE);

    bool covers = false;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi))
    {
        const int tol = 3;
        covers = (rc.left <= mi.rcMonitor.left + tol && rc.top <= mi.rcMonitor.top + tol &&
                  rc.right >= mi.rcMonitor.right - tol && rc.bottom >= mi.rcMonitor.bottom - tol);
    }

    std::wstring why;
    if (brwClass)
        why = L"类名 Brw:（广播窗口）";
    else if (covers && (style & WS_CAPTION) != WS_CAPTION)
        why = (ex & WS_EX_TOPMOST) ? L"远志进程 + 覆盖整屏 + 无标题栏 + 置顶" : L"远志进程 + 覆盖整屏 + 无标题栏";
    else
        return TRUE;

    WatchWindowHit hit;
    hit.hwnd = hwnd;
    hit.pid = pid;
    hit.cls = cls;
    wchar_t title[512] = {0};
    GetWindowTextW(hwnd, title, 512);
    hit.title = title;
    hit.exePath = exePath;
    hit.style = style;
    hit.exStyle = ex;
    hit.rect = rc;
    hit.why = why;
    out->push_back(hit);
    return TRUE;
}

int RunWatch(const std::wstring& outDir, DWORD durationSec, DWORD intervalSec, bool includeAllModules)
{
    if (intervalSec < 1)
        intervalSec = 1;
    if (durationSec == 0)
        durationSec = 0;   /* 0 = 一直挂到 Ctrl+C */

    yz::EnsureDirectory(outDir);
    g_watchLogPath = yz::JoinPath(outDir, L"YZProbe-watch-" + yz::NowStamp() + L".log");
    SetConsoleCtrlHandler(WatchConsoleHandler, TRUE);

    WatchLog(yz::Format(L"watch 启动：间隔 %u 秒，时长 %s，输出目录 %s",
                        intervalSec, durationSec ? yz::Format(L"%u 秒", durationSec).c_str() : L"不限（Ctrl+C 结束）",
                        outDir.c_str()));
    WatchLog(L"触发条件：新远志进程出现 / 任何进程加载 ExdDtDup·ExdElite·ExdPaint / "
             L"ScreenRecoder.exe 出现 / 出现 Brw: 或覆盖整屏的无边框远志窗口");
    WatchLog(L"另每分钟一条心跳；每条触发都会落一份完整报告（文件名带 -watch-<原因>）");

    const ULONGLONG startTick = GetTickCount64();
    const DWORD heartbeatEvery = (60 + intervalSec - 1) / intervalSec;
    const DWORD fullSweepEvery = (60 + intervalSec - 1) / intervalSec;
    /* "新进程观察窗"：进程起来之后头 30 秒每隔一轮都重扫它的模块。
       这条是必需的——现场那种 helper 往往是先启动、过几秒才把 ExdDtDup 加载进来，
       只在"第一次看到这个 pid"时扫一次必然漏掉（本机测试就复现了这个漏）。 */
    const DWORD youngTicks = (30 + intervalSec - 1) / intervalSec;
    DWORD   tick = 0;
    unsigned reports = 0;
    std::map<DWORD, DWORD>   firstSeenTick;
    std::set<DWORD>          knownPids;
    std::set<std::wstring>   reported;      /* pid+组件 去重 */
    std::set<HWND>           knownWindows;

    while (InterlockedCompareExchange(&g_watchStop, 0, 0) == 0)
    {
        if (durationSec != 0 && (GetTickCount64() - startTick) / 1000 >= durationSec)
            break;

        tick++;
        const bool fullSweep = (tick % fullSweepEvery) == 1;

        std::vector<ProbeProcBrief> procs;
        ProbeSnapshotProcesses(procs);

        struct Hit
        {
            DWORD        pid;
            std::wstring name;
            std::wstring path;
            std::wstring module;
            std::wstring tag;              /* window / gdi / capture / proc */
            bool         viaNewProcess;
        };
        std::vector<Hit> hits;
        std::set<DWORD>  hitPids;          /* 同一个进程一轮只留一条：组件命中优先于"进程出现" */

        for (size_t i = 0; i < procs.size(); i++)
        {
            const ProbeProcBrief& b = procs[i];
            bool isNew = knownPids.find(b.pid) == knownPids.end();
            if (isNew)
            {
                knownPids.insert(b.pid);
                firstSeenTick[b.pid] = tick;
            }
            const bool isYoung = (tick - firstSeenTick[b.pid]) <= youngTicks;

            if (yz::IsCaptureStackProcessName(b.name))
            {
                const std::wstring key = yz::Format(L"gdi:%u", b.pid);
                if (reported.find(key) == reported.end())
                {
                    reported.insert(key);
                    Hit h; h.pid = b.pid; h.name = b.name; h.path = b.path;
                    h.module = L"ScreenRecoder.exe（GDI 抓屏 helper）"; h.tag = L"gdi";
                    h.viaNewProcess = isNew;
                    hits.push_back(h);
                    hitPids.insert(b.pid);
                }
            }

            /* 抓采集组件：新进程立刻查；已知进程只在"可疑名字"上查（成本低）；
               每 60 秒做一次全进程兜底扫（覆盖"我们启动前就已经加载"的情形）。 */
            const bool worthCheck = isNew || isYoung || fullSweep || yz::IsVendorCaptureRelatedName(b.name);
            if (worthCheck)
            {
                std::wstring modName;
                std::wstring modPath;
                if (ProbeScanCaptureModule(b.pid, &modName, &modPath))
                {
                    const std::wstring key = yz::Format(L"cap:%u:%s", b.pid, modName.c_str());
                    if (reported.find(key) == reported.end())
                    {
                        reported.insert(key);
                        Hit h; h.pid = b.pid; h.name = b.name; h.path = modPath.empty() ? b.path : modPath;
                        h.module = modName; h.tag = L"capture"; h.viaNewProcess = isNew;
                        hits.push_back(h);
                        hitPids.insert(b.pid);
                    }
                }
            }

            /* 新出现的远志进程本身也值得留一份（客户端被守卫重启、换进程等） */
            if (isNew && yz::IsVendorCaptureRelatedName(b.name) &&
                hitPids.find(b.pid) == hitPids.end())
            {
                const std::wstring key = yz::Format(L"proc:%u", b.pid);
                if (reported.find(key) == reported.end())
                {
                    reported.insert(key);
                    Hit h; h.pid = b.pid; h.name = b.name; h.path = b.path;
                    h.module = L"（进程出现）"; h.tag = L"proc"; h.viaNewProcess = true;
                    hits.push_back(h);
                    hitPids.insert(b.pid);
                }
            }
        }

        /* 窗口触发 */
        std::vector<WatchWindowHit> wins;
        EnumWindows(WatchWindowProc, reinterpret_cast<LPARAM>(&wins));
        for (size_t i = 0; i < wins.size(); i++)
        {
            if (knownWindows.find(wins[i].hwnd) != knownWindows.end())
                continue;
            knownWindows.insert(wins[i].hwnd);
            Hit h;
            h.pid = wins[i].pid;
            h.name = yz::FileNameOf(wins[i].exePath);
            h.path = wins[i].exePath;
            h.module = yz::Format(L"窗口 %p 类=%s 样式=0x%08X/0x%08X 矩形=%d,%d,%d,%d 标题=%s（%s）",
                                  wins[i].hwnd, wins[i].cls.c_str(),
                                  static_cast<unsigned>(wins[i].style),
                                  static_cast<unsigned>(wins[i].exStyle),
                                  wins[i].rect.left, wins[i].rect.top,
                                  wins[i].rect.right, wins[i].rect.bottom,
                                  wins[i].title.c_str(), wins[i].why.c_str());
            h.tag = L"window";
            h.viaNewProcess = true;
            hits.push_back(h);
        }

        /* 触发 → 立刻落一份完整报告 */
        for (size_t i = 0; i < hits.size(); i++)
        {
            const std::wstring& tag = hits[i].tag;
            ProbeData data;
            ProbeCollect(data, includeAllModules);
            data.notes.insert(data.notes.begin(), yz::Format(
                L"watch 触发（%s）：PID=%u %s；%s", tag.c_str(), hits[i].pid,
                hits[i].name.c_str(), hits[i].module.c_str()));
            if (hits[i].viaNewProcess)
                data.notes.insert(data.notes.begin() + 1, L"该进程是本轮 watch 期间新出现的");

            const std::wstring mid = yz::NowStampEx() + L"-watch-" + tag;
            std::wstring jsonPath;
            if (WriteReports(outDir, data, mid, &jsonPath, nullptr))
                reports++;

            WatchLog(yz::Format(L"触发落盘[%s]：PID=%u %s %s → %s", tag.c_str(), hits[i].pid,
                                hits[i].name.c_str(), hits[i].module.c_str(), jsonPath.c_str()));
            for (size_t n = 0; n < data.notes.size(); n++)
            {
                if (yz::ContainsNoCase(data.notes[n], L"采集组件扫描") ||
                    yz::ContainsNoCase(data.notes[n], L"图形采集栈 PID") ||
                    yz::ContainsNoCase(data.notes[n], L"GDI 抓屏 helper"))
                {
                    WatchLog(L"  " + data.notes[n]);
                }
            }
        }

        if ((tick % heartbeatEvery) == 0)
        {
            size_t vendorCount = 0;
            for (size_t i = 0; i < procs.size(); i++)
            {
                if (yz::IsYuanzhiInstallPath(procs[i].path) ||
                    yz::IsVendorCaptureRelatedName(procs[i].name))
                {
                    vendorCount++;
                }
            }
            WatchLog(yz::Format(L"心跳：进程 %zu 个（远志相关 %zu），已落盘报告 %u 份%s",
                                procs.size(), vendorCount, reports,
                                fullSweep ? L"，本轮做了全进程采集组件兜底扫" : L""));
        }

        Sleep(intervalSec * 1000);
    }

    WatchLog(yz::Format(L"watch 结束：共落盘 %u 份报告，日志 %s", reports, g_watchLogPath.c_str()));
    SetConsoleCtrlHandler(WatchConsoleHandler, FALSE);
    return 0;
}
} /* namespace */

int wmain(int argc, wchar_t** argv)
{
    /* 控制台按 UTF-8 输出，CRT 也要用 UTF-8 区域设置，否则宽字符会退化成 '?' */
    SetConsoleOutputCP(CP_UTF8);
    setlocale(LC_ALL, ".UTF8");

    std::wstring outDir = yz::GetExeDir();
    bool includeAllModules = false;
    bool watchMode = false;
    DWORD watchDuration = 0;
    DWORD watchInterval = 3;
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
        else if (arg == L"--watch")
        {
            watchMode = true;
        }
        else if (arg == L"--duration" && i + 1 < argc)
        {
            watchDuration = static_cast<DWORD>(_wtoi(argv[++i]));
        }
        else if (arg == L"--interval" && i + 1 < argc)
        {
            watchInterval = static_cast<DWORD>(_wtoi(argv[++i]));
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
            wprintf(L"用法: YZProbe.exe [-o 输出目录] [--modules] [--access <pid>] [--watch [--duration 秒] [--interval 秒]] [--no-pause]\n");
            wprintf(L"  --modules       采集所有进程的模块（默认只采集远志相关进程）\n");
            wprintf(L"  --access <pid>  对指定进程做注入能力探测（保护级别/句柄实得权限/逐权限位/读/分配/写/线程句柄）\n");
            wprintf(L"  --watch         常驻触发式采集：新远志进程/采集组件加载/疑似广播窗口出现时立刻落一份报告\n");
            wprintf(L"  --duration <秒>  --watch 的时长（默认 0 = 一直挂到 Ctrl+C）\n");
            wprintf(L"  --interval <秒>  --watch 的轮询间隔（默认 3）\n");
            wprintf(L"  双击运行时结束会等你按回车；脚本里用 --no-pause 跳过\n");
            if (!noPause)
                yz::PauseIfSoleConsole();
            return 0;
        }
    }

    if (watchMode)
        return RunWatch(outDir, watchDuration, watchInterval, includeAllModules);

    wprintf(L"YZProbe 正在采集（管理员权限下信息最完整）...\n");

    ProbeData data;
    ProbeCollect(data, includeAllModules);
    if (accessPid != 0)
    {
        ProbeAccessProbePid(accessPid, data);
        data.notes.push_back(yz::Format(L"按 --access 参数额外探测了 PID=%u", accessPid));
    }

    const std::wstring mid = yz::NowStamp();
    std::wstring jsonPath;
    std::wstring mdPath;
    WriteReports(outDir, data, mid, &jsonPath, &mdPath);

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

