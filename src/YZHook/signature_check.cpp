#include "signature_check.h"

#include "../../third_party/minhook/src/hde/hde32.h"

#include <stdarg.h>
#include <stdio.h>

namespace
{
void SetWhy(wchar_t* why, SIZE_T whyLen, const wchar_t* fmt, ...)
{
    if (why == nullptr || whyLen == 0)
        return;
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(why, whyLen, _TRUNCATE, fmt, args);
    va_end(args);
}

/* 该指令是否访问 [esp+4]（x86 下第一个栈参数）。
   编码上一定是 mod=01/10 + rm=100(SIB) + base=ESP + index=none。 */
bool RefersEspPlus4(const hde32s& hs)
{
    if (!(hs.flags & F_MODRM) || !(hs.flags & F_SIB))
        return false;
    if (hs.modrm_rm != 4 || hs.sib_base != 4 || hs.sib_index != 4)
        return false;
    if (hs.modrm_mod == 1 && (hs.flags & F_DISP8) && hs.disp.disp8 == 4)
        return true;
    if (hs.modrm_mod == 2 && (hs.flags & F_DISP32) && hs.disp.disp32 == 4)
        return true;
    return false;
}
} /* namespace */

namespace yzhook
{
bool SignatureLooksCallable(const unsigned char* code, SIZE_T maxLen, bool expectArgRead,
                            wchar_t* why, SIZE_T whyLen)
{
#ifdef _WIN64
    (void)code;
    (void)maxLen;
    (void)expectArgRead;
    SetWhy(why, whyLen, L"64 位构建不启用（远志的钩子模块只有 x86 版本）");
    return false;
#else
    if (code == nullptr || maxLen < 3)
    {
        SetWhy(why, whyLen, L"可读长度不足（%u 字节）", static_cast<unsigned>(maxLen));
        return false;
    }

    bool argReadSeen = false;
    const DWORD kArgProbeBytes = 48;   /* 只在函数开头找栈参数读取 */

    SIZE_T off = 0;
    while (off < maxLen)
    {
        hde32s hs;
        ZeroMemory(&hs, sizeof(hs));
        const unsigned int len = hde32_disasm(code + off, &hs);
        if (len == 0 || (hs.flags & (F_ERROR | F_ERROR_OPCODE | F_ERROR_LENGTH | F_ERROR_LOCK | F_ERROR_OPERAND)))
        {
            SetWhy(why, whyLen, L"指令流解析失败（偏移 %u）", static_cast<unsigned>(off));
            return false;
        }

        if (expectArgRead && !argReadSeen && off < kArgProbeBytes && RefersEspPlus4(hs))
            argReadSeen = true;

        const unsigned char op = hs.opcode;
        if (op == 0xC3)   /* ret */
        {
            if (expectArgRead && !argReadSeen)
            {
                SetWhy(why, whyLen, L"函数开头没有读取 [esp+4]，与「1 个栈参数」的假设不符");
                return false;
            }
            return true;
        }
        if (op == 0xC2)   /* ret imm16 */
        {
            if (hs.imm.imm16 != 0)
            {
                SetWhy(why, whyLen, L"以 ret %u 收尾（会弹掉 %u 字节栈参数），与假设不符",
                       hs.imm.imm16, hs.imm.imm16);
                return false;
            }
            if (expectArgRead && !argReadSeen)
            {
                SetWhy(why, whyLen, L"函数开头没有读取 [esp+4]，与「1 个栈参数」的假设不符");
                return false;
            }
            return true;
        }
        if (op == 0xCA || op == 0xCB)   /* far ret */
        {
            SetWhy(why, whyLen, L"以远返回收尾，无法确认调用约定");
            return false;
        }
        if (op == 0xE9 || op == 0xEB || op == 0xEA ||
            (op == 0xFF && (hs.flags & F_MODRM) && (hs.modrm_reg == 4 || hs.modrm_reg == 5)))
        {
            SetWhy(why, whyLen, L"函数以跳转结尾（可能是 thunk），无法确认调用约定");
            return false;
        }

        off += len;
    }

    SetWhy(why, whyLen, L"在 %u 字节内没有遇到 ret", static_cast<unsigned>(maxLen));
    return false;
#endif
}
} /* namespace yzhook */
