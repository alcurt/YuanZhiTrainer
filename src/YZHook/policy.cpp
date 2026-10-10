#include "policy.h"

#include "yz_hook_state.h"

#include <stddef.h>
#include <sddl.h>
#include <vector>

/* ============================ v0.6.13 的重要决定 ============================
   本文件原来做三件事：备份 / 周期性把被远志改写的"HKCU 策略项"改回自由状态 / 退出时还原。
   2026-09-29 机房实测（G3）证明这套东西**没有作用对象**：
     学生 hive（S-1-5-21-…-500）里一条策略值都没有；有值的只有 .DEFAULT / S-1-5-18
     ——而那正是我们自己写进去的（注入侧自报 `PolicyHive: 本进程 HKCU → \REGISTRY\USER\.DEFAULT`）。
   也就是说远志的封锁不走 HKCU 策略，而我们那套写入落在**默认用户模板**上：
   进程被硬杀时"退出还原"不执行，0/1 就留在模板里被之后新建的 profile 继承。
   收益为零、代价是在别人机器上留下持续副作用，所以 v0.6.13 起**移除全部写入**，
   只保留下面这个纯只读的落点自报——它的诊断价值还在（现场一眼能看出目标进程的 HKCU 指向哪），
   而且是判断"某个值到底是谁写的"的第一视角证据。
   ========================================================================== */

namespace
{
/* ---------- HKCU 落点自报（复核 P1.2 的第一视角证据） ----------
   本文件用 HKEY_CURRENT_USER，而它跑在**目标进程**里：Yistart.exe 以 SYSTEM 运行时，
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

/* SID 对应的 hive 是否已加载？没加载的话 HKCU 会自动落到 .DEFAULT。 */
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

    YZLOGI(L"PolicyHive: 本进程 HKCU → %s（令牌 SID=%s，该 hive 已加载=%s）；"
           L"本程序不写策略（v0.6.13 起只报落点，见 policy.cpp 顶部说明）",
           path.c_str(), sidText, hiveLoaded ? L"是" : L"否");

    if (!sid.empty() && !hiveLoaded)
    {
        YZLOGW(L"PolicyHive: 令牌 SID 对应的 hive 未加载 —— HKCU 会落到 .DEFAULT。"
               L"本程序不再写任何策略值，这一行只用于判读别人写的值落在哪一家");
    }
}
} /* namespace yzhook */
