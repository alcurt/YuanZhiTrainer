#pragma once
//
// 抓屏子进程标记的解析（v0.6.32）。
//
// 为什么单独抽出来：这段逻辑原来内联在 ipc.cpp 的管道线程里，只能靠"起主程序 + 真的拉起
// 子进程"才验得到（而主程序要求管理员）。抽成纯函数后可以用 tests\unit 里的单元测试直接跑，
// 不需要管理员，也不依赖任何进程/日志。
//
// 标记从哪来：hooks_exec.cpp 在 CreateProcessW/A 返回后认出"厂商拉起的抓屏子进程"
// （机房实测：学生端亲手拉起 rundll32.exe + KsFiles\ExdDtDup.dll,RunCapture），
// 在审计结果串尾追加 "[抓屏子进程 pid=N]"，主程序再从镜像回来的日志里认领它。
//
#include <windows.h>

#include <stdlib.h>
#include <string>

namespace yzcap
{

/* 从一行日志里解出 [抓屏子进程 pid=N] 的 N。解不到返回 false。
   规则：取标记后到第一个 ']' 之间的一段，必须**全是数字**且不为 0（超 DWORD 也视为解不到）。 */
inline bool ParseCaptureChildMarker(const std::wstring& text, DWORD* pidOut)
{
    if (pidOut == nullptr)
        return false;
    *pidOut = 0;

    static const wchar_t kMark[] = L"[抓屏子进程 pid=";
    const size_t at = text.find(kMark);
    if (at == std::wstring::npos)
        return false;

    const size_t from = at + (sizeof(kMark) / sizeof(wchar_t)) - 1;   /* 去掉结尾的 L'\0' */
    const size_t to   = text.find(L']', from);
    if (to == std::wstring::npos || to <= from)
        return false;

    const std::wstring digits = text.substr(from, to - from);
    for (size_t i = 0; i < digits.size(); ++i)
    {
        if (digits[i] < L'0' || digits[i] > L'9')
            return false;
    }
    const unsigned long long v = wcstoull(digits.c_str(), nullptr, 10);
    if (v == 0 || v > 0xFFFFFFFFull)
        return false;

    *pidOut = static_cast<DWORD>(v);
    return true;
}

} /* namespace yzcap */