#pragma once
//
// 主动拔钩：调用远志自身导出的卸载入口，去掉"注入之前"就已经装好的钩子。
//
#include <windows.h>

namespace yzhook
{
/* 发起一次主动拔钩（异步：工作线程里跑，主线程只做有上限的等待）。
   waitMs 是等待工作线程的上限（毫秒），0 表示不等待。返回"已成功调用"的入口数量（0..3）。
   同一时刻只会有一个工作线程；上一轮没结束（例如 KillHook 内部卡住）时本轮直接跳过。 */
DWORD NativeUnhookClientHooks(DWORD waitMs);

/* 周期性入口：按 2 秒节流重试，直到"已加载但还没成功调用"的目标都被处理过。
   动机：注入往往发生在远志加载 KeyboardHook.dll / ExdHooks.dll **之前**，
   原来的一次性 latch 会让这条路线静默失效（2026-09-23 复核 P1.3）。 */
void NativeUnhookTick();
} /* namespace yzhook */
