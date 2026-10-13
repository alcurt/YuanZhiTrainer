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

/* 冻结开关（v0.6.33）：开着时 AcquireNextFrame 不再转发，而是**按调用方 timeout 诚实等待**后
   返回 DXGI_ERROR_WAIT_TIMEOUT —— 调用方拿不到新帧，画面就停在最后一帧（§27 本机排练过：
   忙等会打到 3.5M 次/秒，诚实等待后降到 ~32 次/秒而冻结仍有效）。 */
void DupTraceSetFreeze(bool on);
bool DupTraceIsFrozen();   /* v0.6.36：供补充通道判断"是否已经冻着"，避免每轮重复打日志 */
} /* namespace yzhook */

#endif /* YZ_HOOKS_DUPTRACE_H */
