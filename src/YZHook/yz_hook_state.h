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
/* 当前生效的功能开关（YZ_FLAG_*），由主程序下发的配置驱动 */
extern volatile LONG  g_flags;
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
