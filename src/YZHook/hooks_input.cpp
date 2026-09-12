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
bool g_hooksReady = false;

/* 是否需要拦截该 SetWindowsHookEx 调用 */
bool ShouldBlockHook(int idHook, HINSTANCE hMod)
{
    if ((yzhook::g_flags & YZ_FLAG_INPUT_UNLOCK) == 0)
        return false;

    bool interesting = false;
    switch (idHook)
    {
    case WH_KEYBOARD_LL:
    case WH_MOUSE_LL:
    case WH_KEYBOARD:
    case WH_MOUSE:
    case WH_GETMESSAGE:
    case WH_CALLWNDPROC:
        interesting = true;
        break;
    default:
        return false;
    }
    if (!interesting)
        return false;

    /* 线程内钩子（hMod == NULL）一定来自本进程；DLL 钩子则看模块路径 */
    if (hMod == nullptr)
        return true;
    return yzhook::IsModulePathUnderTargetDir(hMod);
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
    if (ShouldBlockHook(idHook, hMod))
    {
        InterlockedIncrement(&g_blockedHookCount);
        yzhook::SendLogToHost(YZ_LOG_INFO,
            yz::Format(L"已拦截键盘/鼠标钩子安装: idHook=%d thread=%u", idHook, threadId).c_str());
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
    if (ShouldBlockHook(idHook, hMod))
    {
        InterlockedIncrement(&g_blockedHookCount);
        yzhook::SendLogToHost(YZ_LOG_INFO,
            yz::Format(L"已拦截键盘/鼠标钩子安装(A): idHook=%d thread=%u", idHook, threadId).c_str());
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
    if (yzhook::g_flags & YZ_FLAG_INPUT_UNLOCK)
    {
        InterlockedIncrement(&g_blockedHookCount);
        SetLastError(ERROR_ACCESS_DENIED);
        result = FALSE;
    }
    else
    {
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
    ok = HookAttach("input", L"user32.dll", "SetWindowsHookExW", reinterpret_cast<void*>(&Hook_SetWindowsHookExW), enableNow) && ok;
    ok = HookAttach("input", L"user32.dll", "SetWindowsHookExA", reinterpret_cast<void*>(&Hook_SetWindowsHookExA), enableNow) && ok;
    ok = HookAttach("input", L"user32.dll", "RegisterHotKey", reinterpret_cast<void*>(&Hook_RegisterHotKey), enableNow) && ok;
    ok = HookAttach("input", L"user32.dll", "SystemParametersInfoW", reinterpret_cast<void*>(&Hook_SystemParametersInfoW), enableNow) && ok;
    ok = HookAttach("input", L"user32.dll", "ClipCursor", reinterpret_cast<void*>(&Hook_ClipCursor), enableNow) && ok;
    ok = HookAttach("input", L"user32.dll", "BlockInput", reinterpret_cast<void*>(&Hook_BlockInput), enableNow) && ok;
    ok = HookAttach("input", L"user32.dll", "keybd_event", reinterpret_cast<void*>(&Hook_keybd_event), enableNow) && ok;
    ok = HookAttach("input", L"user32.dll", "mouse_event", reinterpret_cast<void*>(&Hook_mouse_event), enableNow) && ok;
    ok = HookAttach("input", L"user32.dll", "SendInput", reinterpret_cast<void*>(&Hook_SendInput), enableNow) && ok;
    ok = HookAttach("input", L"user32.dll", "SetCursorPos", reinterpret_cast<void*>(&Hook_SetCursorPos), enableNow) && ok;

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

    if (g_realSetWindowsHookExW == nullptr)
    {
        YZLOGE(L"InputHooksInstall: SetWindowsHookExW 原始指针为空，取消输入 hook");
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
