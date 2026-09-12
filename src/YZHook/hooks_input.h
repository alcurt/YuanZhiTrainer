#pragma once
#include <windows.h>

namespace yzhook
{
bool  InputHooksInstall(bool enableNow);
void  InputHooksShutdown();
void  InputEnforceTick();        /* 引擎每秒调用：强制解除鼠标裁剪等 */
DWORD InputBlockedHookCount();
void  InputResetCounters();
} /* namespace yzhook */
