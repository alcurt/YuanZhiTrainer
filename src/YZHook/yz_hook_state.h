#pragma once
//
// Hook DLL 内部共享状态与工具。
//
#include <windows.h>

#include "yz_protocol.h"
#include "yz_log.h"
#include "yz_util.h"

namespace yzhook
{
/* 当前生效的功能开关（YZ_FLAG_*），由主程序下发的配置驱动。
   注意：这里**只有功能位**（ApplyEffectiveFlags 会 & YZ_FLAG_FUNCTION_MASK），
   控制位（YZ_CFG_*）不在这里——判断控制位要读下面这个独立变量。 */
extern volatile LONG  g_flags;
/* 消息层钩子开关（ini 项 BlockMessageHooks / 控制位 YZ_CFG_BLOCK_MESSAGE_HOOKS）。
   默认 1 = 保持 v0.6.11 之前的行为（把 WH_GETMESSAGE/WH_CALLWNDPROC 也换成空过程）。
   为什么不能直接读 g_flags：控制位被 YZ_FLAG_FUNCTION_MASK 剔除了——v0.6.13 的第一版
   就是这么写的，后果是这个开关**永远为 0**、消息层钩子从此再没被换过
   （回归阶段九第一次跑就抓出来了，见 开发记录.md §20）。 */
extern volatile LONG  g_blockMessageHooks;
extern volatile LONG  g_examMode;
extern DWORD          g_windowPercent;
extern volatile LONG  g_windowizeCount;
extern volatile LONG  g_hooksInstalled;
extern DWORD          g_lastError;
extern std::wstring   g_targetDir;     /* 远志安装目录（小写） */

/* ---- 重入保护：所有 hook 回调进入前必须 TryEnter，退出时 Leave ---- */
bool TryEnterHook();
void LeaveHook();

/* ---- 与主程序通信 ---- */
void SendLogToHost(int level, const wchar_t* text);
void SendStatusToHost(DWORD opcode);

/* ---- 工具 ---- */
bool IsModulePathUnderTargetDir(HMODULE mod);
bool IsWindowOfThisProcess(HWND hwnd);
void LogHooked(const wchar_t* fmt, ...);
} /* namespace yzhook */
