#include "capturewatch.h"

#include "app.h"

#include "yz_capture.h"
#include "yz_log.h"
#include "yz_util.h"

#include <map>
#include <set>
#include <vector>

#include <tlhelp32.h>

namespace
{
struct ProcInfo
{
    DWORD        pid;
    std::wstring name;
    std::wstring path;
};

struct CaptureHitRecord
{
    DWORD        pid;
    std::wstring name;
    std::wstring module;
    std::wstring modulePath;
    DWORD        firstSeenTick;
    DWORD        lastLoggedTick;
};

const DWORD kScanIntervalMs  = 30000;    /* 常规扫描节流 */
const DWORD kFullSweepMs     = 300000;   /* 全进程兜底扫：5 分钟 */
const DWORD kYoungWindowMs   = 30000;    /* 新进程观察窗：这段时间内每轮都重扫 */
const DWORD kRelogIntervalMs = 300000;   /* 同一命中重复记日志的节流 */

DWORD                        g_lastScanTick    = 0;
DWORD                        g_lastFullSweep   = 0;
std::map<DWORD, DWORD>       g_firstSeen;          /* pid -> 首次看到的时间 */
std::map<std::wstring, CaptureHitRecord> g_hits;   /* "pid:module" -> 记录 */

bool SnapshotProcesses(std::vector<ProcInfo>& out)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (pe.th32ProcessID == 0)
                continue;
            ProcInfo p;
            p.pid  = pe.th32ProcessID;
            p.name = pe.szExeFile;
            p.path = yz::GetProcessImagePath(p.pid);
            out.push_back(p);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return !out.empty();
}

bool ScanCaptureModule(DWORD pid, std::wstring* outName, std::wstring* outPath)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    bool found = false;
    MODULEENTRY32W me;
    ZeroMemory(&me, sizeof(me));
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do
        {
            if (!yz::IsCaptureStackModule(me.szModule))
                continue;
            if (outName != nullptr) *outName = me.szModule;
            if (outPath != nullptr) *outPath = me.szExePath;
            found = true;
            break;
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

bool ReadModuleNames(DWORD pid, std::vector<std::wstring>& out)
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
            out.push_back(me.szModule);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return !out.empty();
}

const wchar_t* ModuleRoute(const std::wstring& module)
{
    if (yz::ContainsNoCase(module, L"ExdDtDup"))   return L"DXGI 桌面复制路径（冻结画面必须钩 IDXGIOutputDuplication）";
    if (yz::ContainsNoCase(module, L"ExdElite"))   return L"疑似 DXGI（ExdDtDup 的加载者）";
    if (yz::ContainsNoCase(module, L"ExdPaint"))   return L"疑似 DXGI（广播画面组件，引用 ExdElite）";
    if (yz::ContainsNoCase(module, L"ScreenRecoder")) return L"GDI 抓屏 helper（独立进程，需另行注入）";
    return L"未知采集组件";
}

void RecordHit(const ProcInfo& p, const std::wstring& module, const std::wstring& modulePath,
               DWORD now, bool* firstTime)
{
    const std::wstring key = yz::Format(L"%u:%s", p.pid, module.c_str());
    std::map<std::wstring, CaptureHitRecord>::iterator it = g_hits.find(key);
    if (it != g_hits.end())
    {
        it->second.lastLoggedTick = now;
        if (firstTime != nullptr)
            *firstTime = false;
        return;
    }

    CaptureHitRecord rec;
    rec.pid            = p.pid;
    rec.name           = p.name;
    rec.module         = module;
    rec.modulePath     = modulePath;
    rec.firstSeenTick  = now;
    rec.lastLoggedTick = now;
    g_hits[key] = rec;
    if (firstTime != nullptr)
        *firstTime = true;
}

/* 扫一轮。fullSweep=true 时不管名字都扫；否则只扫远志相关名字与"新进程"。 */
void ScanOnce(bool fullSweep, std::vector<std::wstring>* logLines)
{
    const DWORD now = GetTickCount();
    std::vector<ProcInfo> procs;
    if (!SnapshotProcesses(procs))
        return;

    for (size_t i = 0; i < procs.size(); i++)
    {
        if (g_firstSeen.find(procs[i].pid) == g_firstSeen.end())
            g_firstSeen[procs[i].pid] = now;
    }

    bool firstAutoExport = false;
    for (size_t i = 0; i < procs.size(); i++)
    {
        const ProcInfo& p = procs[i];
        std::wstring module;
        std::wstring modulePath;
        bool hit = false;

        if (yz::IsCaptureStackProcessName(p.name))
        {
            module = p.name;
            hit = true;
        }

        const bool young    = (now - g_firstSeen[p.pid]) <= kYoungWindowMs;
        const bool vendorish = yz::IsVendorCaptureRelatedName(p.name) || yz::IsYuanzhiInstallPath(p.path);
        if (!hit && (fullSweep || young || vendorish))
        {
            std::wstring mod;
            std::wstring modPath;
            if (ScanCaptureModule(p.pid, &mod, &modPath))
            {
                module = mod;
                modulePath = modPath;
                hit = true;
            }
        }

        if (!hit)
            continue;

        bool firstTime = false;
        RecordHit(p, module, modulePath, now, &firstTime);
        if (!firstTime)
            continue;

        const std::wstring line = yz::Format(
            L"采集栈监控: PID=%u %s 出现采集组件 %s（%s）→ %s",
            p.pid, p.name.c_str(), module.c_str(),
            (modulePath.empty() ? p.path.c_str() : modulePath.c_str()),
            ModuleRoute(module));
        YZLOGI(L"%s", line.c_str());
        /* v0.6.32 第二触发点：采集栈监控命中即触发"按 PID 注入"（与 CreateProcessW 那条并行，
           机房实测这条与厂商用哪条 API 拉起抓屏进程无关；重复注入幂等）。 */
        if (g_app.cfg.injectCaptureChild)
        UiAppendLog(yz::kLogInfo, line);
        /* v0.6.32 第二触发点：**采集栈监控命中**也要触发"按 PID 注入"。
           为什么要有这第二条：机房实测（2026-10-12）这个监控 4 次都准确抓到了
           rundll32.exe + ExdDtDup.dll，它与"厂商用哪条 API 拉起抓屏进程"无关；
           而第一条（CreateProcessW 钩子识别 spawn）依赖那次调用经过我们的钩子——
           本机验证时它就没生效过。两条都留，谁先命中谁动手（重复注入是幂等的）。 */
        if (g_app.cfg.injectCaptureChild)
            OnCaptureChildSeen(p.pid, line);
        if (logLines != nullptr)
            logLines->push_back(line);
        firstAutoExport = true;
    }

    /* 只留最近出现过的 pid，避免长时间运行后 map 无限增长 */
    if (g_firstSeen.size() > 4096)
    {
        std::set<DWORD> alive;
        for (size_t i = 0; i < procs.size(); i++)
            alive.insert(procs[i].pid);
        for (std::map<DWORD, DWORD>::iterator it = g_firstSeen.begin(); it != g_firstSeen.end();)
        {
            if (alive.find(it->first) == alive.end())
                it = g_firstSeen.erase(it);
            else
                ++it;
        }
    }

    if (firstAutoExport)
        AutoExportOnCaptureOnce(L"首次发现采集组件（ExdDtDup/ScreenRecoder 等）");
}

std::wstring DumpLines()
{
    std::vector<ProcInfo> procs;
    std::wstring out;

    if (!SnapshotProcesses(procs))
    {
        out += L"（进程枚举失败）\r\n";
        return out;
    }

    out += yz::Format(L"== 采集栈快照 ==\r\n扫描时刻: %s，进程 %zu 个\r\n",
                      yz::NowStampEx().c_str(), procs.size());

    if (g_hits.empty())
    {
        out += L"本次运行期间尚未观察到采集组件加载（ExdDtDup/ExdElite/ExdPaint/ScreenRecoder）。\r\n";
    }
    else
    {
        out += L"本次运行期间观察到的采集组件：\r\n";
        for (std::map<std::wstring, CaptureHitRecord>::const_iterator it = g_hits.begin();
             it != g_hits.end(); ++it)
        {
            const CaptureHitRecord& r = it->second;
            out += yz::Format(L"  PID=%u %s → %s（%s）→ %s\r\n", r.pid, r.name.c_str(),
                              r.module.c_str(), r.modulePath.c_str(), ModuleRoute(r.module));
        }
    }

    /* 当前这一刻的逐进程判定：只对远志相关进程与"正加载了采集组件"的进程出完整模块表，
       否则这份快照会大到没人看。 */
    size_t detailed = 0;
    for (size_t i = 0; i < procs.size(); i++)
    {
        const ProcInfo& p = procs[i];
        const bool vendorish = yz::IsVendorCaptureRelatedName(p.name) || yz::IsYuanzhiInstallPath(p.path);
        if (!vendorish)
            continue;

        std::vector<std::wstring> modules;
        const bool readable = ReadModuleNames(p.pid, modules);
        const yz::CaptureStackInfo info = yz::CaptureStackProbe(modules, p.name, p.path);
        out += L"  " + yz::CaptureStackLine(p.pid, p.name, info) + L"\r\n";
        if (readable)
        {
            std::wstring interesting;
            for (size_t k = 0; k < modules.size(); k++)
            {
                const std::wstring& m = modules[k];
                if (yz::ContainsNoCase(m, L".ads") || yz::ContainsNoCase(m, L"Exd") ||
                    yz::ContainsNoCase(m, L"Hook") || yz::ContainsNoCase(m, L"Player") ||
                    yz::ContainsNoCase(m, L"d3d") || yz::ContainsNoCase(m, L"dxgi") ||
                    yz::ContainsNoCase(m, L"Rtp") || yz::ContainsNoCase(m, L"Svf"))
                {
                    interesting += m + L" ";
                }
            }
            if (!interesting.empty())
                out += L"    关键模块: " + interesting + L"\r\n";
        }
        detailed++;
    }
    if (detailed == 0)
        out += L"  （当前没有远志相关进程在跑）\r\n";
    out += L"\r\n";
    return out;
}
} /* namespace */

void capturewatch::Tick()
{
    if (g_app.examMode)
        return;   /* 考试模式：连同审计一起停，这里也不采集 */

    const DWORD now = GetTickCount();
    if (g_lastScanTick != 0 && (now - g_lastScanTick) < kScanIntervalMs)
        return;
    g_lastScanTick = now;

    const bool fullSweep = (g_lastFullSweep == 0) || ((now - g_lastFullSweep) >= kFullSweepMs);
    if (fullSweep)
        g_lastFullSweep = now;

    ScanOnce(fullSweep, nullptr);
}

std::wstring capturewatch::Dump()
{
    return DumpLines();
}

void capturewatch::Reset()
{
    g_lastScanTick  = 0;
    g_lastFullSweep = 0;
    g_firstSeen.clear();
    g_hits.clear();
}

