#pragma once
//
// 厂商采集栈的识别与判词（YZProbe 与 YZTrainer 共用）。
//
// 为什么抽成公共模块：现场会有两份东西描述同一件事——探针报告里的"图形采集栈"
// 与主程序诊断包里的采集栈快照。两边如果各写一套判词，迟早会出现"同一台机器
// 两份报告说法不一致"的情况，判读的人要先怀疑工具再看数据。这里只放纯逻辑
// （模块名 → 判词），不做任何采集，谁拿到模块名谁就能用。
//
// 背景（2026-10-11 本机静态侦察，见 开发记录.md）：
//   厂商有两套采集实现：
//     * DXGI：KsFiles\ExdDtDup.dll（依赖 d3d11、走 IDXGIOutput1::DuplicateOutput），
//             由 KsFiles\ExdElite.dll 动态加载，ExdElite 又被 Organs\eck.ads /
//             KsFiles\ExdPaint.dll 引用；
//     * GDI ：ScreenRecoder.exe（MFC42 时代，导入 BitBlt/StretchBlt/GetDIBits），
//             被 Organs\Rmdesk.ads（远程桌面/监视）、Organs\CmdProc.ads、ProxyM.exe 引用。
//   所以"教师监视时到底走哪条"必须看现场进程与模块，不能只看磁盘上有没有 ExdDtDup.dll。
//
#include <windows.h>

#include <string>
#include <vector>

namespace yz
{
/* 采集组件模块名（basename，不区分大小写） */
bool IsCaptureStackModule(const std::wstring& moduleName);

/* 采集组件宿主进程名（ScreenRecoder.exe 这类独立抓屏 exe） */
bool IsCaptureStackProcessName(const std::wstring& exeName);

/* 进程名是否属于我们要盯的远志进程（探针/主程序各自的完整名单更宽，
   这里只覆盖"与采集栈有关"的那几个，用于决定要不要为它枚举模块） */
bool IsVendorCaptureRelatedName(const std::wstring& exeName);

struct CaptureStackInfo
{
    bool modulesReadable    = false;   /* 模块枚举是否成功：false 时"内存未见 X"不构成证据 */
    bool hasExdDtDup        = false;   /* DXGI 桌面复制实现已加载 */
    bool hasExdElite        = false;   /* ExdDtDup 的加载者 */
    bool hasExdPaint        = false;   /* 广播画面组件，引用 ExdElite */
    bool hasScreenRecoder   = false;   /* 进程本身就是 ScreenRecoder.exe */
    bool hasD3D11           = false;
    bool hasDxgi            = false;
    bool hasDwmapi          = false;
    bool hasGdi32           = false;
    bool diskExdDtDup       = false;   /* <exe 目录>\KsFiles\ExdDtDup.dll 在磁盘上 */
};

/* 用"模块名列表 + exe 路径/文件名"算出判定结果 */
CaptureStackInfo CaptureStackProbe(const std::vector<std::wstring>& moduleNames,
                                   const std::wstring& exeName,
                                   const std::wstring& exePath);

/* 一行判词（现场日志/报告直接贴这行，两边格式一致）：
   图形采集栈 PID=14924 Yistart.exe：内存 ExdDtDup.dll=否 d3d11.dll=否 …；磁盘 KsFiles\ExdDtDup.dll=在；<verdict> */
std::wstring CaptureStackLine(DWORD pid, const std::wstring& exeName, const CaptureStackInfo& info);

/* <exe 目录>\KsFiles\ExdDtDup.dll 是否存在 */
bool CaptureStackDiskPresent(const std::wstring& exePath);
} /* namespace yz */
