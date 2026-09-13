//
// 主动拔钩：调用远志自身导出的卸载入口，去掉"注入之前"就已装好的钩子。
//
// 事实依据（本机 V9.0 Student 静态反汇编，ImageBase 0x10000000）：
//   KeyboardHook.dll!KillHook      RVA 0x1630  无参；收尾 pop edi / pop esi / ret
//       对共享节 HookData 里的句柄逐个 UnhookWindowsHookEx，再停掉辅助线程。
//   ExdHooks.dll!UnSetExdHooks     RVA 0x11E0  1 个参数（cdecl），以 ret 收尾
//       传 0 即进入工作分支——远志自己在 0x1000117F 就是 push 0 后调用它；
//       工作分支为 EnumWindows(清理回调) + 三次 UnhookWindowsHookEx + 清状态字。
//   ExdHooks.dll!UnSetExdHooks2    RVA 0x1290  1 个参数（cdecl），以 ret 收尾
//       参数必须等于 GetCurrentThreadId()（远志内部调用同样传调用者自己的 tid）；
//       工作分支把线程级 WH_CALLWNDPROC 钩子 UnhookWindowsHookEx 掉。
//
// 两个必须注意的坑：
//   1) KillHook 结尾是 SetEvent(退出事件) + WaitForSingleObject(线程句柄, INFINITE)。
//      万一远志的辅助线程不退出，直接在引擎线程里调用就会把我们永久挂住。所以整串
//      拔钩放到独立工作线程，主线程只做有上限的等待：最坏情况是多一个卡住的线程，
//      引擎循环与日志照常。
//   2) 机房版本可能与本机不同。调用外部代码前依次确认：模块已加载、导出存在、
//      函数以 ret / ret 0 收尾（出现 ret imm16 != 0 说明"无栈参数"假设不成立）、
//      单参数函数开头确实读了 [esp+4]。任一条不满足就记 ERROR 并放弃（fail-closed）。
//
#include "native_unhook.h"

#include "signature_check.h"
#include "yz_hook_state.h"

#include <string.h>

namespace
{
const DWORD kScanBytes = 384;   /* KillHook 实测约 229 字节，留足余量 */

struct Target
{
    const wchar_t* module;        /* GetModuleHandleW 用模块基名 */
    const char*    proc;          /* 导出名 */
    const wchar_t* label;         /* 日志用 */
    bool           takesArg;      /* true = 1 个 DWORD 参数（cdecl） */
    bool           argIsSelfTid;  /* true = 传 GetCurrentThreadId()，false = 传 0 */
    bool           called;        /* 已成功调用 */
};

Target g_targets[] =
{
    { L"KeyboardHook.dll", "KillHook",       L"KeyboardHook!KillHook()",            false, false, false },
    { L"ExdHooks.dll",     "UnSetExdHooks",  L"ExdHooks!UnSetExdHooks(0)",          true,  false, false },
    { L"ExdHooks.dll",     "UnSetExdHooks2", L"ExdHooks!UnSetExdHooks2(本线程tid)", true,  true,  false },
};

LONG g_started = 0;
LONG g_called  = 0;

typedef void  (__cdecl *PFN_NoArg)(void);
typedef DWORD (__cdecl *PFN_OneArg)(DWORD);

/* SEH 保护：远志的代码万一出错，只让这一次调用失败，不带走宿主进程。
   注意 GetExceptionCode() 是内部函数，只能写在 __except 过滤表达式里，
   所以这里用"过滤表达式里赋值 + 处理块里返回"的惯用写法，不能抽成函数。 */
bool CallNoArg(PFN_NoArg fn, DWORD* sehCode)
{
    __try
    {
        fn();
        return true;
    }
    __except (*sehCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool CallOneArg(PFN_OneArg fn, DWORD arg, DWORD* result, DWORD* sehCode)
{
    __try
    {
        *result = fn(arg);
        return true;
    }
    __except (*sehCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

/* 目标地址是否整段可读，避免扫描越过映射边界触发访问违例。 */
bool RangeReadable(const void* p, SIZE_T bytes, SIZE_T* availBytes)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;

    const DWORD prot = mbi.Protect & 0xFF;
    const bool readable = (prot == PAGE_READONLY || prot == PAGE_READWRITE ||
                           prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
                           prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY);
    if (!readable)
        return false;

    const BYTE* base = static_cast<const BYTE*>(mbi.BaseAddress);
    const BYTE* cur  = static_cast<const BYTE*>(p);
    const BYTE* end  = base + mbi.RegionSize;
    *availBytes = (cur + bytes <= end) ? bytes : static_cast<SIZE_T>(end - cur);
    return *availBytes > 0;
}

DWORD WINAPI WorkerProc(LPVOID)
{
    DWORD called = 0;
    for (size_t i = 0; i < sizeof(g_targets) / sizeof(g_targets[0]); i++)
    {
        Target& t = g_targets[i];

        HMODULE mod = GetModuleHandleW(t.module);
        if (mod == nullptr)
        {
            YZLOGD(L"NativeUnhook: %s 所在模块未加载，跳过", t.label);
            continue;
        }

        FARPROC proc = GetProcAddress(mod, t.proc);
        if (proc == nullptr)
        {
            YZLOGW(L"NativeUnhook: 模块已加载但找不到导出 %S，跳过", t.proc);
            continue;
        }

        const BYTE* code = reinterpret_cast<const BYTE*>(proc);
        SIZE_T avail = 0;
        if (!RangeReadable(code, kScanBytes, &avail) || avail < 16)
        {
            YZLOGE(L"NativeUnhook: %s 代码段不可读，放弃调用", t.label);
            continue;
        }

        const SIZE_T scanLen = (avail < kScanBytes) ? avail : kScanBytes;
        wchar_t why[192] = {0};
        if (!yzhook::SignatureLooksCallable(code, scanLen, t.takesArg, why, ARRAYSIZE(why)))
        {
            YZLOGE(L"NativeUnhook: %s 未通过调用约定自检，放弃调用：%s", t.label, why);
            continue;
        }

        DWORD seh = 0;
        if (!t.takesArg)
        {
            if (CallNoArg(reinterpret_cast<PFN_NoArg>(proc), &seh))
            {
                t.called = true;
                called++;
                YZLOGI(L"NativeUnhook: 已调用 %s", t.label);
                yzhook::SendLogToHost(YZ_LOG_INFO, yz::Format(L"主动拔钩: 已调用 %s", t.label).c_str());
            }
            else
            {
                YZLOGE(L"NativeUnhook: 调用 %s 触发异常 0x%08X", t.label, seh);
                yzhook::SendLogToHost(YZ_LOG_ERROR,
                    yz::Format(L"主动拔钩: 调用 %s 触发异常 0x%08X", t.label, seh).c_str());
            }
        }
        else
        {
            const DWORD arg   = t.argIsSelfTid ? GetCurrentThreadId() : 0;
            DWORD       ret   = 0;
            if (CallOneArg(reinterpret_cast<PFN_OneArg>(proc), arg, &ret, &seh))
            {
                t.called = true;
                called++;
                YZLOGI(L"NativeUnhook: 已调用 %s -> 返回 %u", t.label, ret);
                yzhook::SendLogToHost(YZ_LOG_INFO,
                    yz::Format(L"主动拔钩: 已调用 %s -> 返回 %u", t.label, ret).c_str());
            }
            else
            {
                YZLOGE(L"NativeUnhook: 调用 %s 触发异常 0x%08X", t.label, seh);
                yzhook::SendLogToHost(YZ_LOG_ERROR,
                    yz::Format(L"主动拔钩: 调用 %s 触发异常 0x%08X", t.label, seh).c_str());
            }
        }
    }

    InterlockedExchange(&g_called, static_cast<LONG>(called));
    if (called > 0)
        YZLOGI(L"NativeUnhook: 完成，成功调用 %u 个远志自带卸载入口", called);
    else
        YZLOGW(L"NativeUnhook: 没有入口被调用（模块未加载或未通过自检，见上文日志）");

    return 0;
}
} /* namespace */

namespace yzhook
{
DWORD NativeUnhookClientHooks(DWORD waitMs)
{
#ifdef _WIN64
    (void)waitMs;
    YZLOGI(L"NativeUnhook: 64 位构建不启用（远志的钩子模块只有 x86 版本）");
    return 0;
#else
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0)
        return static_cast<DWORD>(InterlockedCompareExchange(&g_called, 0, 0));

    HANDLE thread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    if (thread == nullptr)
    {
        InterlockedExchange(&g_started, 0);
        YZLOGE(L"NativeUnhook: 工作线程创建失败");
        return 0;
    }

    YZLOGI(L"NativeUnhook: 开始主动拔钩（调用远志自带导出）");
    if (waitMs != 0)
        WaitForSingleObject(thread, waitMs);
    CloseHandle(thread);

    return static_cast<DWORD>(InterlockedCompareExchange(&g_called, 0, 0));
#endif
}
} /* namespace yzhook */
