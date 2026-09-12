#include "hookmgr.h"

#include "yz_util.h"

#include <string>
#include <vector>

#include "MinHook.h"

namespace
{
struct Entry
{
    std::string  group;
    std::wstring module;
    std::string  proc;
    void*        target;
    void*        detour;
    void*        original;
    bool         enabled;
};

CRITICAL_SECTION     g_cs;
bool                 g_csInit = false;
bool                 g_mhInit = false;
std::vector<Entry>   g_entries;

void Lock()
{
    if (g_csInit)
        EnterCriticalSection(&g_cs);
}

void Unlock()
{
    if (g_csInit)
        LeaveCriticalSection(&g_cs);
}
} /* namespace */

namespace yzhook
{
bool HookInit()
{
    if (!g_csInit)
    {
        InitializeCriticalSection(&g_cs);
        g_csInit = true;
    }

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        YZLOGE(L"HookInit: MH_Initialize 失败, status=%d", static_cast<int>(st));
        return false;
    }
    g_mhInit = true;
    return true;
}

void HookUninit()
{
    HookDetachAll();
    if (g_mhInit)
    {
        MH_Uninitialize();
        g_mhInit = false;
    }
}

bool HookAttach(const char* group, const wchar_t* moduleName, const char* procName,
                void* detour, bool enableNow)
{
    if (detour == nullptr)
        return false;

    Lock();
    for (size_t i = 0; i < g_entries.size(); i++)
    {
        if (g_entries[i].detour == detour)
        {
            Unlock();
            return true; /* 已附加过 */
        }
    }
    Unlock();

    HMODULE mod = GetModuleHandleW(moduleName);
    if (mod == nullptr)
        mod = LoadLibraryW(moduleName);
    if (mod == nullptr)
    {
        YZLOGE(L"HookAttach: 加载模块 %s 失败", moduleName);
        return false;
    }

    void* target = reinterpret_cast<void*>(GetProcAddress(mod, procName));
    if (target == nullptr)
    {
        YZLOGD(L"HookAttach: 模块 %s 未导出 %S，跳过", moduleName, procName);
        return false;
    }

    void* original = nullptr;
    MH_STATUS st = MH_CreateHook(target, detour, &original);
    if (st == MH_ERROR_ALREADY_CREATED)
    {
        YZLOGW(L"HookAttach: %S 已被其它 hook 占用", procName);
        return true;
    }
    if (st != MH_OK)
    {
        YZLOGE(L"HookAttach: MH_CreateHook(%S) 失败, status=%d", procName, static_cast<int>(st));
        return false;
    }

    if (enableNow)
    {
        st = MH_EnableHook(target);
        if (st != MH_OK)
        {
            YZLOGE(L"HookAttach: MH_EnableHook(%S) 失败, status=%d", procName, static_cast<int>(st));
            MH_RemoveHook(target);
            return false;
        }
    }

    Entry e;
    e.group    = (group != nullptr) ? group : "";
    e.module   = moduleName;
    e.proc     = procName;
    e.target   = target;
    e.detour   = detour;
    e.original = original;
    e.enabled  = enableNow;

    Lock();
    g_entries.push_back(e);
    Unlock();

    YZLOGD(L"HookAttach: %S %s (enabled=%d)", procName, e.module.c_str(), enableNow ? 1 : 0);
    return true;
}

bool HookSetGroupEnabled(const char* group, bool enable)
{
    if (group == nullptr)
        return false;

    Lock();
    std::vector<void*> targets;
    for (size_t i = 0; i < g_entries.size(); i++)
    {
        if (g_entries[i].group == group && g_entries[i].enabled != enable)
            targets.push_back(g_entries[i].target);
    }
    Unlock();

    bool ok = true;
    for (size_t i = 0; i < targets.size(); i++)
    {
        MH_STATUS st = enable ? MH_EnableHook(targets[i]) : MH_DisableHook(targets[i]);
        if (st != MH_OK)
        {
            YZLOGE(L"HookSetGroupEnabled: group=%S enable=%d status=%d",
                   group, enable ? 1 : 0, static_cast<int>(st));
            ok = false;
            continue;
        }
        Lock();
        for (size_t k = 0; k < g_entries.size(); k++)
        {
            if (g_entries[k].target == targets[i])
                g_entries[k].enabled = enable;
        }
        Unlock();
    }
    return ok;
}

bool HookSetEnabledByDetour(void* detour, bool enable)
{
    Lock();
    void* target = nullptr;
    for (size_t i = 0; i < g_entries.size(); i++)
    {
        if (g_entries[i].detour == detour)
        {
            target = g_entries[i].target;
            break;
        }
    }
    Unlock();

    if (target == nullptr)
        return false;

    MH_STATUS st = enable ? MH_EnableHook(target) : MH_DisableHook(target);
    if (st != MH_OK)
        return false;

    Lock();
    for (size_t i = 0; i < g_entries.size(); i++)
    {
        if (g_entries[i].target == target)
            g_entries[i].enabled = enable;
    }
    Unlock();
    return true;
}

DWORD HookActiveCount()
{
    DWORD count = 0;
    Lock();
    for (size_t i = 0; i < g_entries.size(); i++)
    {
        if (g_entries[i].enabled)
            count++;
    }
    Unlock();
    return count;
}

void HookDetachAll()
{
    Lock();
    std::vector<void*> targets;
    for (size_t i = 0; i < g_entries.size(); i++)
        targets.push_back(g_entries[i].target);
    g_entries.clear();
    Unlock();

    for (size_t i = 0; i < targets.size(); i++)
    {
        MH_DisableHook(targets[i]);
        MH_RemoveHook(targets[i]);
    }
}
} /* namespace yzhook */

namespace yzhook
{
void* HookGetOriginal(void* detour)
{
    void* original = nullptr;
    Lock();
    for (size_t i = 0; i < g_entries.size(); i++)
    {
        if (g_entries[i].detour == detour)
        {
            original = g_entries[i].original;
            break;
        }
    }
    Unlock();
    return original;
}
} /* namespace yzhook */
