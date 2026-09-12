#pragma once
#include <windows.h>

namespace yzhook
{
void PolicyBackup();     /* 首次运行时记录原始值 */
void PolicyEnforce();    /* 周期性把被改写的热键/任务管理器策略改回自由状态 */
void PolicyRestore();    /* 退出时恢复原始值 */
} /* namespace yzhook */
