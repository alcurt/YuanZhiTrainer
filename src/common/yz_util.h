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

/* 路径是否属于远志安装目录。同时兼容普通版与网管版：
   "…\YZinfo Multimedia teaching software…" 与网管版默认前缀 "…\GZYZ\…"。 */
bool         IsYuanzhiInstallPath(const std::wstring& path);

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

/* ---- 界面 / DPI ---- */
UINT  GetWindowDpi(HWND hwnd);                    /* 取窗口 DPI；hwnd 可为 nullptr，失败回退 96 */
int   ScaleForDpi(int value, UINT dpi);           /* value * dpi / 96 */
HFONT CreateUiFontForDpi(UINT dpi);               /* 9pt 系统消息字体，按 DPI 换算高度 */

/* ---- 控制台工具用 ---- */
/* 是否“独占”当前控制台（双击运行的情形：只有本进程挂在控制台上）。
   被 PowerShell/cmd 拉起或输出被重定向时返回 false——那时不该停等。 */
bool IsSoleConsoleOwner();
/* 独占控制台时打印提示并等一次回车，避免窗口一闪而过看不到输出。 */
void PauseIfSoleConsole();

/* ---- 杂项 ---- */
unsigned long long Fnv1a64(const void* data, size_t len);
} /* namespace yz */
