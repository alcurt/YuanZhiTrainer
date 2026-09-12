#pragma once
//
// 公共工具：路径、字符串、进程/令牌查询、权限提升。
//
#include <windows.h>
#include <string>
#include <vector>

namespace yz
{
/* ---- 路径 ---- */
std::wstring GetModuleFilePath(HMODULE mod);
std::wstring GetExeDir();
std::wstring GetSelfPath();
std::wstring GetTempDir();
std::wstring JoinPath(const std::wstring& dir, const std::wstring& name);
std::wstring FileNameOf(const std::wstring& path);
std::wstring DirNameOf(const std::wstring& path);
bool         EnsureDirectory(const std::wstring& dir);
bool         FileExists(const std::wstring& path);
bool         IsUnderDir(const std::wstring& path, const std::wstring& dir);

/* ---- 字符串 ---- */
std::wstring ToLower(const std::wstring& s);
bool         ContainsNoCase(const std::wstring& haystack, const std::wstring& needle);
bool         StartsWithNoCase(const std::wstring& s, const std::wstring& prefix);
std::wstring Trim(const std::wstring& s);
std::vector<std::wstring> SplitString(const std::wstring& s, wchar_t sep);
std::wstring Format(const wchar_t* fmt, ...);
std::string  WideToUtf8(const std::wstring& s);
std::wstring Utf8ToWide(const std::string& s);
std::wstring Win32ErrorMessage(DWORD err);
std::wstring NowStamp();      /* 20260912-153000 */
std::wstring NowStampEx();    /* 20260912-153000-123 */

/* ---- 进程与令牌 ---- */
std::wstring GetProcessImagePath(DWORD pid);
bool         IsProcessElevated(bool* outElevated);
bool         IsProcessHandleElevated(HANDLE hProcess, bool* outElevated);
DWORD        GetHandleIntegrityRid(HANDLE hProcess);
std::wstring GetHandleUser(HANDLE hProcess);
bool         EnablePrivilege(const wchar_t* name);
DWORD        SessionIdOfCurrentProcess();
bool         IsProcessAlive(DWORD pid);

/* ---- 杂项 ---- */
unsigned long long Fnv1a64(const void* data, size_t len);
} /* namespace yz */
