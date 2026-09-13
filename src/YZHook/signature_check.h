#pragma once
//
// 调用约定自检：在调用远志自带导出前，确认它是"以 ret / ret 0 收尾"，
// 需要时再确认它确实读取了第一个栈参数 [esp+4]。
//
// 用 vendor 进来的 HDE32（MinHook 自带）做真实指令长度解码，而不是裸字节扫描：
// 裸扫描会被指令里的 ModRM/立即数骗到——实测 ExdHooks!UnSetExdHooks 的
// `3B C3`(cmp eax,ebx) 里就有一个 0xC3 字节，会在偏移 0xD 被误判成 ret。
//
#include <windows.h>

namespace yzhook
{
/* code/maxLen : 函数起始地址与可安全读取的字节数
   expectArgRead: true 表示该导出应带 1 个 DWORD 栈参数（cdecl）
   why/whyLen  : 失败原因（可为 nullptr）
   返回 true 才允许调用。x64 构建恒返回 false（HDE32 只解 32 位指令流）。 */
bool SignatureLooksCallable(const unsigned char* code, SIZE_T maxLen, bool expectArgRead,
                            wchar_t* why, SIZE_T whyLen);
} /* namespace yzhook */
