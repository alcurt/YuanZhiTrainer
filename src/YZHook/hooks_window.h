#pragma once
#include <windows.h>

namespace yzhook
{
bool  WindowHooksInstall(bool enableNow);
void  WindowHooksShutdown();

void  WindowSweepTick();          /* 引擎每 500ms 调用一次 */
bool  WindowIsTracked(HWND hwnd);

void  WindowHooksSetTopmost(bool on);
bool  WindowHooksGetTopmost();

void  WindowHooksResetCount();
DWORD WindowHooksCount();
} /* namespace yzhook */
