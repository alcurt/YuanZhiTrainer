#include "policy.h"

#include "yz_hook_state.h"

namespace
{
/* ⚠ 已知边界（批次 2 待验证，先不动）：
   本文件全部用 HKEY_CURRENT_USER，而这段代码跑在**目标进程**里（Yistart.exe 以
   SYSTEM 运行），所以解析出来的是 SYSTEM 的 hive，不是登录学生那个用户的 hive。
   后果：把 DisableTaskMgr / NoWinKeys 之类"改回来"对登录用户可能根本没生效，
   PolicyRestore 也可能是"执行了但用户侧没变化"。
   2026-09-23 复核 P1.2。验证办法：在被锁的机器上分别读
     HKU\<学生 SID>\Software\Microsoft\Windows\CurrentVersion\Policies\...
   与 SYSTEM 的 hive，看远志到底写在哪一侧。确认后再决定是否引入
   WTSQueryUserToken / 直接操作交互用户 hive —— 注入本身还没稳之前不加这层不确定性。 */
struct Item
{
    HKEY         root;
    const wchar_t* subKey;
    const wchar_t* name;
    DWORD        desired;
    bool         backedUp;
    bool         hadValue;
    DWORD        original;
};

Item g_items[] =
{
    { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",    L"DisableTaskMgr",         0, false, false, 0 },
    { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",    L"DisableLockWorkstation", 0, false, false, 0 },
    { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",    L"DisableChangePassword",  0, false, false, 0 },
    { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",  L"NoWinKeys",              0, false, false, 0 },
    { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\GameDVR",             L"AppCaptureEnabled",      1, false, false, 0 }
};

const size_t g_itemCount = sizeof(g_items) / sizeof(g_items[0]);

bool ReadDword(const Item& item, DWORD* out)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(item.root, item.subKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0;
    DWORD size = sizeof(DWORD);
    DWORD value = 0;
    LONG rc = RegQueryValueExW(key, item.name, nullptr, &type, reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_DWORD)
        return false;
    *out = value;
    return true;
}

bool WriteDword(const Item& item, DWORD value)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(item.root, item.subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc = RegSetValueExW(key, item.name, 0, REG_DWORD,
                             reinterpret_cast<const BYTE*>(&value), sizeof(DWORD));
    RegCloseKey(key);
    return rc == ERROR_SUCCESS;
}

void DeleteValue(const Item& item)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(item.root, item.subKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;
    RegDeleteValueW(key, item.name);
    RegCloseKey(key);
}
} /* namespace */

namespace yzhook
{
void PolicyBackup()
{
    for (size_t i = 0; i < g_itemCount; i++)
    {
        if (g_items[i].backedUp)
            continue;
        DWORD value = 0;
        g_items[i].hadValue = ReadDword(g_items[i], &value);
        g_items[i].original = value;
        g_items[i].backedUp = true;
    }
    YZLOGI(L"PolicyBackup: 已记录 %u 项策略原值", static_cast<unsigned>(g_itemCount));
}

void PolicyEnforce()
{
    if ((g_flags & YZ_FLAG_INPUT_UNLOCK) == 0)
        return;

    for (size_t i = 0; i < g_itemCount; i++)
    {
        DWORD value = 0;
        bool exists = ReadDword(g_items[i], &value);
        if (!exists || value != g_items[i].desired)
        {
            if (WriteDword(g_items[i], g_items[i].desired))
            {
                SendLogToHost(YZ_LOG_INFO,
                    yz::Format(L"恢复被改写的策略项 %s = %u",
                               g_items[i].name, g_items[i].desired).c_str());
            }
        }
    }
}

void PolicyRestore()
{
    for (size_t i = 0; i < g_itemCount; i++)
    {
        if (!g_items[i].backedUp)
            continue;
        if (g_items[i].hadValue)
            WriteDword(g_items[i], g_items[i].original);
        else
            DeleteValue(g_items[i]);
    }
    YZLOGI(L"PolicyRestore: 已恢复原始策略值");
}
} /* namespace yzhook */
