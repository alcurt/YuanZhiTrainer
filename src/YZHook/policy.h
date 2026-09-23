#pragma once
#include <windows.h>

namespace yzhook
{
void PolicyBackup();     /* 首次运行时记录原始值 */
void PolicyEnforce();    /* 周期性把被改写的热键/任务管理器策略改回自由状态 */
void PolicyRestore();    /* 退出时恢复原始值 */
/* 只读自报：本进程的 HKCU 到底解析到哪个 hive（形如 \REGISTRY\USER\S-1-5-18）。
   这段代码跑在目标进程里，所以这是"第一视角"的事实，用来回答
   "策略项到底被写进了哪一家"。纯日志，不改任何状态、不动句柄之外的东西。 */
void PolicyLogCurrentUserHive();
} /* namespace yzhook */
