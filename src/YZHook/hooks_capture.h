#pragma once
#include <windows.h>

namespace yzhook
{
bool CaptureHooksInstall(bool enableNow);
void CaptureHooksShutdown();

/* 抓取当前画面并进入冻结状态；返回是否成功 */
bool CaptureFreeze();
void CaptureUnfreeze();
bool CaptureIsFrozen();

DWORD CaptureRedirectCount();
} /* namespace yzhook */
