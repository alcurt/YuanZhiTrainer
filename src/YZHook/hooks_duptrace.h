//
// DXGI 桌面复制取证钩子（v0.6.21）：只计数，不改变任何行为。
//
#ifndef YZ_HOOKS_DUPTRACE_H
#define YZ_HOOKS_DUPTRACE_H

#include <windows.h>

namespace yzhook
{
/* 装上取证钩子（enableNow=false 直接返回 true，让调用点与其它 hook 模块写法一致）。
   默认不调用它：只有控制位 YZ_CFG_TRACE_DUPLICATION 为 1 时才装。 */
bool DupTraceInstall(bool enableNow);
void DupTraceShutdown();
} /* namespace yzhook */

#endif /* YZ_HOOKS_DUPTRACE_H */
