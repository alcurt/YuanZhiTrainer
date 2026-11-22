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
long CaptureScreenBltCalls();   /* v0.6.87：屏幕源 BitBlt 调用数（冻结替换次数用已有的 CaptureRedirectCount） */

DWORD CaptureRedirectCount();
} /* namespace yzhook */
