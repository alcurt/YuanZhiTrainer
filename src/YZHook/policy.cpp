#include "policy.h"

#include "yz_hook_state.h"

#include <stddef.h>
#include <sddl.h>
#include <vector>

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

/* ---------- HKCU 落点自报（复核 P1.2 的第一视角证据） ----------
   本文件全用 HKEY_CURRENT_USER，而它跑在**目标进程**里：Yistart.exe 以 SYSTEM 运行时，
   解析出来的就是 SYSTEM 的 hive，跟登录学生那一家不是同一个。光靠外部脚本扫 HKU 只能
   推断"谁家被写了"，这里让进程自己报出它的 HKCU 到底叫什么名字。
   纯只读：只开句柄、查名字、关句柄。 */

typedef LONG (NTAPI *PFN_NtQueryKey)(HANDLE, int, PVOID, ULONG, PULONG);

struct KeyNameInformation            /* KEY_NAME_INFORMATION，class = 3 */
{
    ULONG NameLength;                /* **字节数**；且 Name 不以 null 结尾 */
    WCHAR Name[1];
};

/* 关键点：NameLength 是字节数、名字没有结尾 null，所以必须显式按"字符数"构造
   std::wstring；直接当 %ls 打印会越界读，严重时把目标进程带走。 */
std::wstring QueryKeyName(HKEY key)
{
    static PFN_NtQueryKey s_fn       = nullptr;
    static bool           s_resolved = false;
    if (!s_resolved)
    {
        s_resolved = true;
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (nt != nullptr)
            s_fn = reinterpret_cast<PFN_NtQueryKey>(GetProcAddress(nt, "NtQueryKey"));
    }
    if (s_fn == nullptr || key == nullptr)
        return std::wstring();

    std::vector<BYTE> buf(512);
    ULONG             need = 0;
    LONG st = s_fn(key, 3 /*KeyNameInformation*/, buf.data(),
                   static_cast<ULONG>(buf.size()), &need);
    if (st < 0 && need > buf.size())
    {
        buf.assign(need, 0);
        st = s_fn(key, 3, buf.data(), static_cast<ULONG>(buf.size()), &need);
    }
    if (st < 0)
        return std::wstring();

    const KeyNameInformation* info = reinterpret_cast<const KeyNameInformation*>(buf.data());
    if (info->NameLength < sizeof(WCHAR))
        return std::wstring();

    const size_t chars    = info->NameLength / sizeof(WCHAR);
    const size_t maxChars = (buf.size() - offsetof(KeyNameInformation, Name)) / sizeof(WCHAR);
    return std::wstring(info->Name, (chars < maxChars) ? chars : maxChars);
}

std::wstring TokenUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) || token == nullptr)
        return std::wstring();

    DWORD need = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &need);
    std::wstring sid;
    if (need != 0)
    {
        std::vector<BYTE> buf(need);
        if (GetTokenInformation(token, TokenUser, buf.data(), need, &need))
        {
            TOKEN_USER* tu  = reinterpret_cast<TOKEN_USER*>(buf.data());
            LPWSTR      str = nullptr;
            if (ConvertSidToStringSidW(tu->User.Sid, &str) && str != nullptr)
            {
                sid = str;
                LocalFree(str);
            }
        }
    }
    CloseHandle(token);
    return sid;
}

/* SID 对应的 hive 是否已加载？没加载的话 HKCU 会自动落到 .DEFAULT，
   这正是"策略改了半天没效果"的另一种可能。 */
bool HiveLoadedForSid(const std::wstring& sid)
{
    if (sid.empty())
        return false;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS || h == nullptr)
        return false;
    RegCloseKey(h);
    return true;
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

void PolicyLogCurrentUserHive()
{
    /* 第一视角：直接问内核"我这个 HKCU 句柄叫什么名字"。
       RegOpenCurrentUser 的句柄必须显式 RegCloseKey 释放。 */
    std::wstring path;
    HKEY         hk = nullptr;
    if (RegOpenCurrentUser(KEY_READ, &hk) == ERROR_SUCCESS && hk != nullptr)
    {
        path = QueryKeyName(hk);
        RegCloseKey(hk);
    }

    /* 旁证：令牌里的用户 SID，以及那个 hive 是否已加载 */
    const std::wstring sid        = TokenUserSid();
    const bool         hiveLoaded = HiveLoadedForSid(sid);
    const wchar_t*     sidText    = sid.empty() ? L"(未知)" : sid.c_str();

    if (path.empty())
    {
        YZLOGW(L"PolicyHive: 取不到 HKCU 真实路径（RegOpenCurrentUser/NtQueryKey 失败）；"
               L"令牌 SID=%s，该 hive 已加载=%s",
               sidText, hiveLoaded ? L"是" : L"否");
        return;
    }

    YZLOGI(L"PolicyHive: 本进程 HKCU → %s（令牌 SID=%s，该 hive 已加载=%s）",
           path.c_str(), sidText, hiveLoaded ? L"是" : L"否");

    if (!sid.empty() && !hiveLoaded)
    {
        YZLOGW(L"PolicyHive: 令牌 SID 对应的 hive 未加载 —— HKCU 会落到 .DEFAULT，"
               L"策略项读写会写在别处（这正是复核 P1.2 要排除的情形）");
    }
}
} /* namespace yzhook */
