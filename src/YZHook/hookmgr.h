#pragma once
//
// MinHook 薄封装：按组管理 hook，支持整组启用/停用。
//
#include <windows.h>

namespace yzhook
{
bool  HookInit();
void  HookUninit();

/* 附加一个 hook；group 用于整组开关（"window" / "input" / "capture"） */
bool  HookAttach(const char* group, const wchar_t* moduleName, const char* procName,
                 void* detour, bool enableNow);

/* 取回原始函数指针（用于在 detour 内调用原函数） */
void* HookGetOriginal(void* detour);

bool  HookSetGroupEnabled(const char* group, bool enable);
bool  HookSetEnabledByDetour(void* detour, bool enable);
DWORD HookActiveCount();
void  HookDetachAll();
} /* namespace yzhook */
