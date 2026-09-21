#pragma once
//
// 外部窗口纠正（免注入兜底）。
//
// 为什么需要它：机房网管版对 Yistart.exe 做了句柄权限剥夺（ObRegisterCallbacks），
// VirtualAllocEx 直接返回 0x5，进程内 Hook 根本进不去；消息钩子注入也可能被
// 安全软件拦下。可窗口样式是会话级对象，改别人的窗口不需要目标进程配合——
// 于是这里在主程序侧按"与 YZHook 完全相同的结构判据"找远志的无边框全屏窗口，
// 再用跨进程 SetWindowLongPtrW / SetWindowPos 把它改成普通窗口。
//
// 与进程内 Hook 的分工：Hook 生效时跳过它所在的宿主进程（避免两边抢同一个窗口），
// 其余远志进程（以及 Hook 完全不可用的场合）由本模块负责。
//
#include <windows.h>

namespace winfix
{
/* 由看门狗线程按秒调用；内部自带节流、开关与考试模式判断 */
void WinFixTick();

/* 进程内 Hook 已生效的宿主 PID：跳过它。传 0 表示没有 Hook 在跑 */
void WinFixSetSkipPid(DWORD pid);

/* 与进程内 Hook 的"窗口置顶"开关保持一致 */
void WinFixSetTopmost(bool on);

/* 统计（状态栏与诊断包用） */
DWORD WinFixCount();     /* 累计纠正次数 */

/* 一次性取回：纠正次数 / 上一轮候选窗口数 / 最近一次写入是否失败 */
void WinFixStats(DWORD* corrections, DWORD* candidates, bool* writeFailed);
}
