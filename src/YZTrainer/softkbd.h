#pragma once
//
// 软键盘（v0.6.73）。
//
// 动机：现场键盘被锁时，物理按键进不来；实测 Yistart 的用户态里没有任何输入 API（无 keybd_event /
// SendInput / BlockInput），键鼠锁大概率在驱动层。**消息级注入不经过键盘驱动栈**：
// 直接把 WM_KEYDOWN / WM_CHAR 投给焦点窗口，所以只要能点鼠标，就能把字打进去。
//
// 边界：只在用户点击软键盘按钮时投递消息；不常驻、不轮询、不注入任何进程。
//
#include <windows.h>

namespace softkbd {

/* 显示/隐藏软键盘。hwndOwner 用来记"属于谁"，并作为没有外部焦点窗口时的兜底目标。 */
void Toggle(HWND hwndOwner);

/* 关闭并销毁（退出时调用）。 */
void Shutdown();

/* 当前是否显示。 */
bool IsVisible();

}  // namespace softkbd
