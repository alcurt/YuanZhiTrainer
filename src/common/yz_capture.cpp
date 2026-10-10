#include "yz_capture.h"

#include "yz_util.h"

namespace yz
{
namespace
{
bool SameName(const std::wstring& a, const wchar_t* b)
{
    return _wcsicmp(a.c_str(), b) == 0;
}
} /* namespace */

bool IsCaptureStackModule(const std::wstring& moduleName)
{
    if (moduleName.empty())
        return false;
    return SameName(moduleName, L"ExdDtDup.dll") ||
           SameName(moduleName, L"ExdElite.dll") ||
           SameName(moduleName, L"ExdPaint.dll");
}

bool IsCaptureStackProcessName(const std::wstring& exeName)
{
    if (exeName.empty())
        return false;
    return SameName(exeName, L"ScreenRecoder.exe");
}

bool IsVendorCaptureRelatedName(const std::wstring& exeName)
{
    if (exeName.empty())
        return false;
    /* 只列"可能承载采集栈"的几个；Yistart/Nmdeputy 是主客户端与守卫，
       它们更可能只是加载者，但正因为如此也必须检查。 */
    return SameName(exeName, L"Yistart.exe") ||
           SameName(exeName, L"PlayerGUI.exe") ||
           SameName(exeName, L"ExdPaintHelper.exe") ||
           SameName(exeName, L"Nmdeputy.exe") ||
           SameName(exeName, L"ScreenRecoder.exe") ||
           SameName(exeName, L"TEACHCMD.exe") ||
           SameName(exeName, L"PMonitorNO.exe") ||
           SameName(exeName, L"ExdDrvGuard.exe");
}

bool CaptureStackDiskPresent(const std::wstring& exePath)
{
    if (exePath.empty())
        return false;
    const std::wstring dir = DirNameOf(exePath);
    if (dir.empty())
        return false;
    return FileExists(JoinPath(JoinPath(dir, L"KsFiles"), L"ExdDtDup.dll"));
}

CaptureStackInfo CaptureStackProbe(const std::vector<std::wstring>& moduleNames,
                                   const std::wstring& exeName,
                                   const std::wstring& exePath)
{
    CaptureStackInfo info;
    info.modulesReadable = !moduleNames.empty();
    for (size_t i = 0; i < moduleNames.size(); i++)
    {
        const std::wstring& m = moduleNames[i];
        if (SameName(m, L"ExdDtDup.dll"))      info.hasExdDtDup = true;
        else if (SameName(m, L"ExdElite.dll")) info.hasExdElite = true;
        else if (SameName(m, L"ExdPaint.dll")) info.hasExdPaint = true;
        else if (SameName(m, L"d3d11.dll"))    info.hasD3D11 = true;
        else if (SameName(m, L"dxgi.dll"))     info.hasDxgi = true;
        else if (SameName(m, L"dwmapi.dll"))   info.hasDwmapi = true;
        else if (SameName(m, L"gdi32.dll"))    info.hasGdi32 = true;
    }
    info.hasScreenRecoder = IsCaptureStackProcessName(exeName);
    info.diskExdDtDup     = CaptureStackDiskPresent(exePath);
    return info;
}

std::wstring CaptureStackLine(DWORD pid, const std::wstring& exeName, const CaptureStackInfo& info)
{
    const wchar_t* verdict = nullptr;
    std::wstring modulesNote = L"内存";

    if (info.hasExdDtDup)
    {
        verdict = L"采集层=DXGI 桌面复制（ExdDtDup 已加载，冻结画面必须走 DXGI 路径）";
    }
    else if (info.hasScreenRecoder)
    {
        verdict = L"采集层=GDI 抓屏 helper（本进程就是 ScreenRecoder.exe）";
    }
    else if (!info.modulesReadable)
    {
        /* 这一条是 2026-10-11 补的：受保护进程枚举不到模块时，
           过去会被读成"内存没有 ExdDtDup"，与"读不到"混淆。 */
        modulesNote = L"内存（模块枚举失败，以下内存结论不可用）";
        verdict = L"采集层=未知（模块枚举失败；磁盘那份是否在用，本次无法判定）";
    }
    else if (info.hasExdElite || info.hasExdPaint)
    {
        verdict = L"采集层=疑似 DXGI（ExdElite/ExdPaint 已加载，ExdDtDup 尚未出现）";
    }
    else if (info.diskExdDtDup)
    {
        verdict = L"采集层=本机装着 DXGI 实现（KsFiles\\ExdDtDup.dll），但本次没观察到它被加载";
    }
    else if (info.hasD3D11 || info.hasDxgi)
    {
        verdict = L"采集层=疑似 DXGI（d3d11/dxgi 已加载但未见 ExdDtDup）";
    }
    else
    {
        verdict = L"采集层=本进程内未见 DXGI 采集模块（注意 ScreenRecoder.exe 这类独立 helper 不在本进程里）";
    }

    return Format(L"图形采集栈 PID=%u %s：%s ExdDtDup.dll=%s d3d11.dll=%s dxgi.dll=%s "
                  L"dwmapi.dll=%s gdi32.dll=%s；磁盘 KsFiles\\ExdDtDup.dll=%s；%s",
                  pid, exeName.c_str(), modulesNote.c_str(),
                  info.hasExdDtDup ? L"是" : L"否",
                  info.hasD3D11 ? L"是" : L"否",
                  info.hasDxgi ? L"是" : L"否",
                  info.hasDwmapi ? L"是" : L"否",
                  info.hasGdi32 ? L"是" : L"否",
                  info.diskExdDtDup ? L"在" : L"无",
                  verdict);
}
} /* namespace yz */
