#pragma once
//
// 内嵌 Hook 载荷（RCDATA 形式的 YZHook.dll）。
//
// 与 injector.cpp 的分工：
//   injector.cpp 负责"怎么注入"，本模块只负责"注入用的 DLL 从哪来"。
//
#include <windows.h>
#include <string>

/* 把内嵌的 YZHook.dll 释放到磁盘，返回可交给 LoadLibraryW 的绝对路径。
   失败返回空串，并把原因写进 *err（err 可为 nullptr）。 */
std::wstring PayloadEnsureHookDll(std::wstring* err);

/* 内嵌资源的字节数；没有资源返回 0。仅用于日志与诊断。 */
DWORD PayloadEmbeddedSize();

/* 内嵌资源的 PE 架构是否与本进程一致。启动自检用。 */
bool PayloadCheckEmbedded();
