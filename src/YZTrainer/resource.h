#pragma once
//
// 资源 ID 定义。
//
// IDR_HOOK_DLL：以 RCDATA 形式内嵌的注入模块（YZHook.dll）。
// 同一个 ID 在 Win32 / x64 两个平台分别指向对应架构的 DLL，
// 由 YZTrainer.vcxproj 的 YZHOOK_X64 宏切换 yzres.rc 里的路径，
// 运行时再由 payload.cpp 校验 PE Machine 字段，杜绝构建配对错误。
//
#define IDR_HOOK_DLL 0x1001
