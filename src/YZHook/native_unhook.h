#pragma once
//
// 主动拔钩：调用远志自身导出的卸载入口，去掉"注入之前"就已经装好的钩子。
//
#include <windows.h>

namespace yzhook
{
/* 发起一次主动拔钩。进程内一次性：首次调用才真正执行，后续调用直接返回已有结果。
   waitMs 是等待工作线程的上限（毫秒），0 表示不等待。返回成功调用的入口数量。 */
DWORD NativeUnhookClientHooks(DWORD waitMs);
} /* namespace yzhook */
