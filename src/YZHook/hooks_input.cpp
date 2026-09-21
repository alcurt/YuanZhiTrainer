#include "hooks_input.h"

#include "hookmgr.h"
#include "yz_hook_state.h"

#ifndef SPI_SETSCREENSAVERRUNNING
#define SPI_SETSCREENSAVERRUNNING 0x0061
#endif
#ifndef SPI_SETFASTTASKSWITCH
#define SPI_SETFASTTASKSWITCH 0x0026
#endif

namespace
{
typedef HHOOK (WINAPI *PFN_SetWindowsHookExW)(int, HOOKPROC, HINSTANCE, DWORD);
typedef HHOOK (WINAPI *PFN_SetWindowsHookExA)(int, HOOKPROC, HINSTANCE, DWORD);
typedef BOOL  (WINAPI *PFN_RegisterHotKey)(HWND, int, UINT, UINT);
typedef BOOL  (WINAPI *PFN_SystemParametersInfoW)(UINT, UINT, PVOID, UINT);
typedef BOOL  (WINAPI *PFN_ClipCursor)(const RECT*);
typedef BOOL  (WINAPI *PFN_BlockInput)(BOOL);
typedef void  (WINAPI *PFN_keybd_event)(BYTE, BYTE, DWORD, ULONG_PTR);
typedef void  (WINAPI *PFN_mouse_event)(DWORD, DWORD, DWORD, DWORD, ULONG_PTR);
typedef UINT  (WINAPI *PFN_SendInput)(UINT, LPINPUT, int);
typedef BOOL  (WINAPI *PFN_SetCursorPos)(int, int);

PFN_SetWindowsHookExW      g_realSetWindowsHookExW      = nullptr;
PFN_SetWindowsHookExA      g_realSetWindowsHookExA      = nullptr;
PFN_RegisterHotKey         g_realRegisterHotKey         = nullptr;
PFN_SystemParametersInfoW  g_realSystemParametersInfoW  = nullptr;
PFN_ClipCursor             g_realClipCursor             = nullptr;
PFN_BlockInput             g_realBlockInput             = nullptr;
PFN_keybd_event            g_realkeybd_event            = nullptr;
PFN_mouse_event            g_realmouse_event            = nullptr;
PFN_SendInput              g_realSendInput              = nullptr;
PFN_SetCursorPos           g_realSetCursorPos           = nullptr;

volatile LONG g_blockedHookCount = 0;
volatile LONG g_blockedRemoteCount = 0;
volatile LONG g_blockedHotkeyCount = 0;
volatile LONG g_blockedHookLogged = 0;
volatile LONG g_unattributedHookCount = 0;
volatile LONG g_unattributedHookLogged = 0;
bool g_hooksReady = false;

/* 回调地址落在哪个模块里？跨模块判断"这个钩子是不是远志自己装的"靠它。
   GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 对未知/已卸载地址会失败，失败即视为
   无法归属（调用方据此放行并记账），不再像以前那样把所有无模块的线程钩子一律拦掉。 */
bool CallbackInTargetModule(LPCVOID lpfn)
{
    if (lpfn == nullptr)
        return false;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(lpfn), &mod) ||
        mod == nullptr)
        return false;
    return yzhook::IsModulePathUnderTargetDir(mod);
}

/* 拦截日志按次节流：远志失败后会周期性重试，原样每来一条记一行会把日志刷爆 */
void LogHookDecision(const wchar_t* action, int idHook, DWORD threadId, DWORD moduleBase)
{
    const LONG n = InterlockedIncrement(&g_blockedHookLogged);
    if (n > 8 && (n % 50) != 0)
        return;
    yzhook::SendLogToHost(YZ_LOG_INFO,
        yz::Format(L"%s: idHook=%d thread=%u hmod=0x%08X（累计 %d 次）",
                   action, idHook, threadId, moduleBase, n).c_str());
}

/* 是否需要拦截该 SetWindowsHookEx 调用。
   只拦"确实是远志自己装的"钩子：DLL 钩子看模块路径，线程内钩子（hMod == NULL）
   按回调地址反查所属模块。这样同进程里输入法/公共控件/其它组件的合法钩子不再被误伤。 */
bool ShouldBlockHook(int idHook, HOOKPROC lpfn, HINSTANCE hMod)
{
    if ((yzhook::g_flags & YZ_FLAG_INPUT_UNLOCK) == 0)
        return false;

    switch (idHook)
    {
    case WH_KEYBOARD_LL:
    case WH_MOUSE_LL:
    case WH_KEYBOARD:
    case WH_MOUSE:
    case WH_GETMESSAGE:
    case WH_CALLWNDPROC:
        break;
    default:
        return false;
    }

    if (hMod != nullptr)
        return yzhook::IsModulePathUnderTargetDir(hMod);

    if (CallbackInTargetModule(reinterpret_cast<LPCVOID>(lpfn)))
        return true;

    /* 归属不明：放行，但记账 + 有限次日志——机房上一旦发现锁没解开，
       这里就是"是不是被我们放过去了"的第一手证据。 */
    const LONG n = InterlockedIncrement(&g_unattributedHookCount);
    const LONG logged = InterlockedIncrement(&g_unattributedHookLogged);
    if (logged <= 3 || (logged % 50) == 0)
    {
        yzhook::SendLogToHost(YZ_LOG_INFO,
            yz::Format(L"线程内钩子来源无法归属，已放行: idHook=%d lpfn=0x%p（累计 %d 次）",
                       idHook, reinterpret_cast<const void*>(lpfn), n).c_str());
    }
    return false;
}

/* 远志会注册这些组合键来抢/屏蔽系统快捷键。只拦这些"逃生键"，
   不再把同进程里所有 RegisterHotKey 一棍子打死。 */
bool IsLockdownHotkey(UINT modifiers, UINT vk)
{
    const bool alt  = (modifiers & MOD_ALT) != 0;
    const bool ctrl = (modifiers & MOD_CONTROL) != 0;
    const bool shft = (modifiers & MOD_SHIFT) != 0;

    if ((modifiers & MOD_WIN) != 0)                     /* Win / Win+任意键 */
        return true;
    if (alt && (vk == VK_TAB || vk == VK_ESCAPE || vk == VK_F4 || vk == VK_SPACE))
        return true;                                    /* Alt+Tab / Alt+Esc / Alt+F4 / Alt+Space */
    if (ctrl && vk == VK_ESCAPE)                        /* Ctrl+Esc（开始菜单） */
        return true;
    if (ctrl && shft && vk == VK_ESCAPE)                /* Ctrl+Shift+Esc（任务管理器） */
        return true;
    return false;
}

bool IsBlockedSpi(UINT action)
{
    return action == SPI_SETSCREENSAVEACTIVE ||
           action == SPI_SETSCREENSAVERRUNNING ||
           action == SPI_SETFASTTASKSWITCH;
}
} /* namespace */

HHOOK WINAPI Hook_SetWindowsHookExW(int idHook, HOOKPROC lpfn, HINSTANCE hMod, DWORD threadId)
{
    if (!yzhook::TryEnterHook())
        return g_realSetWindowsHookExW(idHook, lpfn, hMod, threadId);

    HHOOK result = nullptr;
    if (ShouldBlockHook(idHook, lpfn, hMod))
    {
        InterlockedIncrement(&g_blockedHookCount);
        LogHookDecision(L"已拦截远志键盘/鼠标钩子安装", idHook, threadId,
                        static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(hMod)));
        SetLastError(ERROR_ACCESS_DENIED);
        result = nullptr;
    }
    else
    {
        result = g_realSetWindowsHookExW(idHook, lpfn, hMod, threadId);
    }

    yzhook::LeaveHook();
    return result;
}

HHOOK WINAPI Hook_SetWindowsHookExA(int idHook, HOOKPROC lpfn, HINSTANCE hMod, DWORD threadId)
{
    if (!yzhook::TryEnterHook())
        return g_realSetWindowsHookExA(idHook, lpfn, hMod, threadId);

    HHOOK result = nullptr;
    if (ShouldBlockHook(idHook, lpfn, hMod))
    {
        InterlockedIncrement(&g_blockedHookCount);
        LogHookDecision(L"已拦截远志键盘/鼠标钩子安装(A)", idHook, threadId,
                        static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(hMod)));
        SetLastError(ERROR_ACCESS_DENIED);
        result = nullptr;
    }
    else
    {
        result = g_realSetWindowsHookExA(idHook, lpfn, hMod, threadId);
    }

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_RegisterHotKey(HWND hwnd, int id, UINT modifiers, UINT vk)
{
    if (!yzhook::TryEnterHook())
        return g_realRegisterHotKey(hwnd, id, modifiers, vk);

    BOOL result = FALSE;
    if ((yzhook::g_flags & YZ_FLAG_INPUT_UNLOCK) && IsLockdownHotkey(modifiers, vk))
    {
        InterlockedIncrement(&g_blockedHotkeyCount);
        yzhook::SendLogToHost(YZ_LOG_INFO,
            yz::Format(L"已拦截远志抢注逃生快捷键: mod=0x%04X vk=0x%02X", modifiers, vk).c_str());
        SetLastError(ERROR_ACCESS_DENIED);
        result = FALSE;
    }
    else
    {
        /* 其余热键放行：同进程里输入法、公共控件、其它组件也都要注册热键 */
        result = g_realRegisterHotKey(hwnd, id, modifiers, vk);
    }

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_SystemParametersInfoW(UINT action, UINT param, PVOID data, UINT flags)
{
    if (!yzhook::TryEnterHook())
        return g_realSystemParametersInfoW(action, param, data, flags);

    BOOL result = FALSE;
    if ((yzhook::g_flags & YZ_FLAG_INPUT_UNLOCK) && IsBlockedSpi(action))
    {
        yzhook::SendLogToHost(YZ_LOG_INFO,
            yz::Format(L"已拦截系统参数改写: action=0x%04X", action).c_str());
        result = TRUE;
    }
    else
    {
        result = g_realSystemParametersInfoW(action, param, data, flags);
    }

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_ClipCursor(const RECT* rect)
{
    if (!yzhook::TryEnterHook())
        return g_realClipCursor(rect);

    BOOL result = FALSE;
    if ((yzhook::g_flags & YZ_FLAG_INPUT_UNLOCK) && rect != nullptr)
    {
        result = g_realClipCursor(nullptr);   /* 强制解除鼠标锁定 */
        yzhook::SendLogToHost(YZ_LOG_INFO, L"已解除鼠标区域锁定(ClipCursor)");
    }
    else
    {
        result = g_realClipCursor(rect);
    }

    yzhook::LeaveHook();
    return result;
}

BOOL WINAPI Hook_BlockInput(BOOL block)
{
    if (!yzhook::TryEnterHook())
        return g_realBlockInput(block);

    BOOL result = FALSE;
    if ((yzhook::g_flags & YZ_FLAG_INPUT_UNLOCK) && block)
    {
        yzhook::SendLogToHost(YZ_LOG_INFO, L"已拦截输入封锁(BlockInput)");
        result = TRUE;
    }
    else
    {
        result = g_realBlockInput(block);
    }

    yzhook::LeaveHook();
    return result;
}

void WINAPI Hook_keybd_event(BYTE vk, BYTE scan, DWORD flags, ULONG_PTR extra)
{
    if (!yzhook::TryEnterHook())
    {
        g_realkeybd_event(vk, scan, flags, extra);
        return;
    }

    if (yzhook::g_flags & YZ_FLAG_BLOCK_REMOTE)
    {
        InterlockedIncrement(&g_blockedRemoteCount);
    }
    else
    {
        g_realkeybd_event(vk, scan, flags, extra);
    }
    yzhook::LeaveHook();
}

UINT WINAPI Hook_SendInput(UINT count, LPINPUT inputs, int size)
{
    if (!yzhook::TryEnterHook())
        return g_realSendInput(count, inputs, size);

    UINT result = 0;
    if (yzhook::g_flags & YZ_FLAG_BLOCK_REMOTE)
    {
        InterlockedIncrement(&g_blockedRemoteCount);
        /* 返回"全部投递成功"而不是 0：调用方（教师端遥控的落地代码）拿不到
           失败信号，就不会去重试或降级到别的输入通道。JiYuTrainer 同样这么做。 */
        result = count;
    }
    else
    {
        result = g_realSendInput(count, inputs, size);
    }

    yzhook::LeaveHook();
    return result;
}

void WINAPI Hook_mouse_event(DWORD flags, DWORD dx, DWORD dy, DWORD data, ULONG_PTR extra)
{
    if (!yzhook::TryEnterHook())
    {
        g_realmouse_event(flags, dx, dy, data, extra);
        return;
    }

    if (yzhook::g_flags & YZ_FLAG_BLOCK_REMOTE)
    {
        InterlockedIncrement(&g_blockedRemoteCount);
    }
    else
    {
        g_realmouse_event(flags, dx, dy, data, extra);
    }
    yzhook::LeaveHook();
}

BOOL WINAPI Hook_SetCursorPos(int x, int y)
{
    if (!yzhook::TryEnterHook())
        return g_realSetCursorPos(x, y);

    BOOL result = FALSE;
    if (yzhook::g_flags & YZ_FLAG_BLOCK_REMOTE)
    {
        InterlockedIncrement(&g_blockedRemoteCount);
        result = TRUE;
    }
    else
    {
        result = g_realSetCursorPos(x, y);
    }

    yzhook::LeaveHook();
    return result;
}

namespace yzhook
{
bool InputHooksInstall(bool enableNow)
{
    if (g_hooksReady)
        return true;

    bool ok = true;

    /* 两阶段安装，理由同 CaptureHooksInstall：绝不能在 g_real* 为空时让钩子生效。 */
    ok = HookAttach("input", L"user32.dll", "SetWindowsHookExW", reinterpret_cast<void*>(&Hook_SetWindowsHookExW), false) && ok;
    ok = HookAttach("input", L"user32.dll", "SetWindowsHookExA", reinterpret_cast<void*>(&Hook_SetWindowsHookExA), false) && ok;
    ok = HookAttach("input", L"user32.dll", "RegisterHotKey", reinterpret_cast<void*>(&Hook_RegisterHotKey), false) && ok;
    ok = HookAttach("input", L"user32.dll", "SystemParametersInfoW", reinterpret_cast<void*>(&Hook_SystemParametersInfoW), false) && ok;
    ok = HookAttach("input", L"user32.dll", "ClipCursor", reinterpret_cast<void*>(&Hook_ClipCursor), false) && ok;
    ok = HookAttach("input", L"user32.dll", "BlockInput", reinterpret_cast<void*>(&Hook_BlockInput), false) && ok;
    ok = HookAttach("input", L"user32.dll", "keybd_event", reinterpret_cast<void*>(&Hook_keybd_event), false) && ok;
    ok = HookAttach("input", L"user32.dll", "mouse_event", reinterpret_cast<void*>(&Hook_mouse_event), false) && ok;
    ok = HookAttach("input", L"user32.dll", "SendInput", reinterpret_cast<void*>(&Hook_SendInput), false) && ok;
    ok = HookAttach("input", L"user32.dll", "SetCursorPos", reinterpret_cast<void*>(&Hook_SetCursorPos), false) && ok;

    g_realSetWindowsHookExW     = reinterpret_cast<PFN_SetWindowsHookExW>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SetWindowsHookExW)));
    g_realSetWindowsHookExA     = reinterpret_cast<PFN_SetWindowsHookExA>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SetWindowsHookExA)));
    g_realRegisterHotKey        = reinterpret_cast<PFN_RegisterHotKey>(HookGetOriginal(reinterpret_cast<void*>(&Hook_RegisterHotKey)));
    g_realSystemParametersInfoW = reinterpret_cast<PFN_SystemParametersInfoW>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SystemParametersInfoW)));
    g_realClipCursor            = reinterpret_cast<PFN_ClipCursor>(HookGetOriginal(reinterpret_cast<void*>(&Hook_ClipCursor)));
    g_realBlockInput            = reinterpret_cast<PFN_BlockInput>(HookGetOriginal(reinterpret_cast<void*>(&Hook_BlockInput)));
    g_realkeybd_event           = reinterpret_cast<PFN_keybd_event>(HookGetOriginal(reinterpret_cast<void*>(&Hook_keybd_event)));
    g_realmouse_event           = reinterpret_cast<PFN_mouse_event>(HookGetOriginal(reinterpret_cast<void*>(&Hook_mouse_event)));
    g_realSendInput             = reinterpret_cast<PFN_SendInput>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SendInput)));
    g_realSetCursorPos          = reinterpret_cast<PFN_SetCursorPos>(HookGetOriginal(reinterpret_cast<void*>(&Hook_SetCursorPos)));

    if (g_realSetWindowsHookExW == nullptr || g_realSetWindowsHookExA == nullptr ||
        g_realRegisterHotKey == nullptr || g_realSystemParametersInfoW == nullptr ||
        g_realClipCursor == nullptr || g_realBlockInput == nullptr ||
        g_realkeybd_event == nullptr || g_realmouse_event == nullptr ||
        g_realSendInput == nullptr || g_realSetCursorPos == nullptr)
    {
        YZLOGE(L"InputHooksInstall: 原始函数指针不完整，输入 hook 保持停用");
        return false;
    }

    if (enableNow && !HookSetGroupEnabled("input", true))
    {
        YZLOGE(L"InputHooksInstall: 启用输入 hook 失败");
        return false;
    }

    g_hooksReady = true;
    YZLOGI(L"InputHooksInstall: 完成 (enableNow=%d)", enableNow ? 1 : 0);
    return ok;
}

void InputHooksShutdown()
{
    g_hooksReady = false;
}

void InputEnforceTick()
{
    if (!(g_flags & YZ_FLAG_INPUT_UNLOCK))
        return;
    if (g_realClipCursor != nullptr)
        g_realClipCursor(nullptr);
    if (g_realBlockInput != nullptr)
        g_realBlockInput(FALSE);
}

DWORD InputBlockedHookCount()
{
    return static_cast<DWORD>(g_blockedHookCount);
}

void InputResetCounters()
{
    g_blockedHookCount = 0;
    g_blockedRemoteCount = 0;
}
} /* namespace yzhook */
