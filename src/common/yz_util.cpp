#include "yz_util.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <wctype.h>

#include <securitybaseapi.h>
#include <string.h>

#pragma comment(lib, "advapi32.lib")

namespace yz
{
std::wstring GetModuleFilePath(HMODULE mod)
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;)
    {
        DWORD got = GetModuleFileNameW(mod, buf.data(), static_cast<DWORD>(buf.size()));
        if (got == 0)
            return std::wstring();
        if (got < buf.size() - 1)
            return std::wstring(buf.data(), got);
        buf.resize(buf.size() * 2);
        if (buf.size() > 32768)
            return std::wstring();
    }
}

std::wstring GetExeDir()
{
    return DirNameOf(GetModuleFilePath(nullptr));
}

std::wstring GetSelfPath()
{
    return GetModuleFilePath(nullptr);
}

std::wstring GetTempDir()
{
    std::vector<wchar_t> buf(MAX_PATH);
    for (;;)
    {
        DWORD got = GetTempPathW(static_cast<DWORD>(buf.size()), buf.data());
        if (got == 0)
            return L"C:\\Windows\\Temp\\";
        if (got < buf.size())
            return std::wstring(buf.data(), got);
        buf.resize(buf.size() * 2);
    }
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty())
        return name;
    std::wstring out = dir;
    wchar_t last = out[out.size() - 1];
    if (last != L'\\' && last != L'/')
        out += L'\\';
    out += name;
    return out;
}

std::wstring FileNameOf(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return path;
    return path.substr(pos + 1);
}

std::wstring DirNameOf(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos)
        return std::wstring();
    return path.substr(0, pos);
}

bool EnsureDirectory(const std::wstring& dir)
{
    if (dir.empty())
        return false;
    if (CreateDirectoryW(dir.c_str(), nullptr))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool FileExists(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsUnderDir(const std::wstring& path, const std::wstring& dir)
{
    if (path.empty() || dir.empty())
        return false;
    std::wstring p = ToLower(path);
    std::wstring d = ToLower(dir);
    if (d[d.size() - 1] != L'\\')
        d += L'\\';
    return p.compare(0, d.size(), d) == 0;
}

std::wstring ToLower(const std::wstring& s)
{
    std::wstring out = s;
    for (size_t i = 0; i < out.size(); i++)
        out[i] = static_cast<wchar_t>(towlower(out[i]));
    return out;
}

bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle)
{
    if (needle.empty())
        return true;
    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix)
{
    if (s.size() < prefix.size())
        return false;
    return ToLower(s).compare(0, prefix.size(), ToLower(prefix)) == 0;
}

std::wstring Trim(const std::wstring& s)
{
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == L' ' || s[b] == L'\t' || s[b] == L'\r' || s[b] == L'\n'))
        b++;
    while (e > b && (s[e - 1] == L' ' || s[e - 1] == L'\t' || s[e - 1] == L'\r' || s[e - 1] == L'\n'))
        e--;
    return s.substr(b, e - b);
}

std::vector<std::wstring> SplitString(const std::wstring& s, wchar_t sep)
{
    std::vector<std::wstring> out;
    std::wstring cur;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (s[i] == sep)
        {
            std::wstring t = Trim(cur);
            if (!t.empty())
                out.push_back(t);
            cur.clear();
        }
        else
        {
            cur += s[i];
        }
    }
    std::wstring t = Trim(cur);
    if (!t.empty())
        out.push_back(t);
    return out;
}

std::wstring Format(const wchar_t* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int need = _vscwprintf(fmt, args);
    va_end(args);
    if (need <= 0)
        return std::wstring();

    std::vector<wchar_t> buf(static_cast<size_t>(need) + 1, L'\0');
    va_start(args, fmt);
    _vsnwprintf_s(buf.data(), buf.size(), _TRUNCATE, fmt, args);
    va_end(args);
    return std::wstring(buf.data());
}

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty())
        return std::string();
    int need = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0)
        return std::string();
    std::string out(static_cast<size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty())
        return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0)
        return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

std::wstring Win32ErrorMessage(DWORD err)
{
    wchar_t* buf = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring out;
    if (len != 0 && buf != nullptr)
        out.assign(buf, len);
    if (buf != nullptr)
        LocalFree(buf);
    out = Trim(out);
    return Format(L"0x%08X %s", err, out.c_str());
}

std::wstring NowStamp()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Format(L"%04d%02d%02d-%02d%02d%02d", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
}

std::wstring NowStampEx()
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    return Format(L"%04d%02d%02d-%02d%02d%02d-%03d", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

std::wstring GetProcessImagePath(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
        return std::wstring();
    std::vector<wchar_t> buf(MAX_PATH * 2);
    DWORD size = static_cast<DWORD>(buf.size());
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf.data(), &size))
        out.assign(buf.data(), size);
    CloseHandle(h);
    return out;
}

bool IsProcessHandleElevated(HANDLE hProcess, bool* outElevated)
{
    if (outElevated != nullptr)
        *outElevated = false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(hProcess, TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION info;
    DWORD ret = 0;
    bool ok = false;
    if (GetTokenInformation(token, TokenElevation, &info, sizeof(info), &ret))
    {
        if (outElevated != nullptr)
            *outElevated = info.TokenIsElevated != 0;
        ok = true;
    }
    CloseHandle(token);
    return ok;
}

bool IsProcessElevated(bool* outElevated)
{
    return IsProcessHandleElevated(GetCurrentProcess(), outElevated);
}

DWORD GetHandleIntegrityRid(HANDLE hProcess)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(hProcess, TOKEN_QUERY, &token))
        return 0;
    DWORD ret = 0;
    DWORD rid = 0;
    if (!GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &ret) &&
        GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        std::vector<BYTE> buf(ret);
        if (GetTokenInformation(token, TokenIntegrityLevel, buf.data(), ret, &ret))
        {
            TOKEN_MANDATORY_LABEL* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
            if (label->Label.Sid != nullptr)
            {
                DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
                if (count > 0)
                    rid = *GetSidSubAuthority(label->Label.Sid, count - 1);
            }
        }
    }
    CloseHandle(token);
    return rid;
}

std::wstring GetHandleUser(HANDLE hProcess)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(hProcess, TOKEN_QUERY, &token))
        return std::wstring();
    DWORD ret = 0;
    std::wstring out;
    GetTokenInformation(token, TokenUser, nullptr, 0, &ret);
    if (ret != 0)
    {
        std::vector<BYTE> buf(ret);
        if (GetTokenInformation(token, TokenUser, buf.data(), ret, &ret))
        {
            TOKEN_USER* user = reinterpret_cast<TOKEN_USER*>(buf.data());
            wchar_t name[256] = {0};
            wchar_t domain[256] = {0};
            DWORD nameLen = 256;
            DWORD domainLen = 256;
            SID_NAME_USE use;
            if (LookupAccountSidW(nullptr, user->User.Sid, name, &nameLen, domain, &domainLen, &use))
                out = Format(L"%s\\%s", domain, name);
        }
    }
    CloseHandle(token);
    return out;
}

bool EnablePrivilege(const wchar_t* name)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    LUID luid;
    bool ok = false;
    if (LookupPrivilegeValueW(nullptr, name, &luid))
    {
        TOKEN_PRIVILEGES tp;
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        if (AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr))
            ok = (GetLastError() == ERROR_SUCCESS);
    }
    CloseHandle(token);
    return ok;
}

DWORD SessionIdOfCurrentProcess()
{
    DWORD sid = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &sid))
        return 0;
    return sid;
}

bool IsProcessAlive(DWORD pid)
{
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr)
        return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

UINT GetWindowDpi(HWND hwnd)
{
    typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
    static PFN_GetDpiForWindow s_getDpiForWindow = nullptr;
    static bool               s_resolved         = false;
    if (!s_resolved)
    {
        s_resolved = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr)
            s_getDpiForWindow =
                reinterpret_cast<PFN_GetDpiForWindow>(GetProcAddress(user32, "GetDpiForWindow"));
    }
    if (s_getDpiForWindow != nullptr && hwnd != nullptr)
    {
        const UINT dpi = s_getDpiForWindow(hwnd);
        if (dpi != 0)
            return dpi;
    }

    /* 回退：桌面 DC 的 LOGPIXELSY。声明了 DPI 感知的进程会拿到系统 DPI。 */
    UINT dpi = 96;
    HDC dc = GetDC(nullptr);
    if (dc != nullptr)
    {
        const int v = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(nullptr, dc);
        if (v > 0)
            dpi = static_cast<UINT>(v);
    }
    return dpi != 0 ? dpi : 96;
}

int ScaleForDpi(int value, UINT dpi)
{
    return MulDiv(value, static_cast<int>(dpi), 96);
}

HFONT CreateUiFontForDpi(UINT dpi)
{
    LOGFONTW lf;
    ZeroMemory(&lf, sizeof(lf));

    NONCLIENTMETRICSW ncm;
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
    {
        lf = ncm.lfMessageFont;   /* 取系统消息字体族（中文系统下是雅黑系） */
    }
    else
    {
        lf.lfHeight  = -12;
        lf.lfWeight  = FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        wcsncpy_s(lf.lfFaceName, LF_FACESIZE, L"MS Shell Dlg", _TRUNCATE);
    }

    lf.lfHeight  = -MulDiv(9, static_cast<int>(dpi), 72);   /* 9pt */
    lf.lfQuality = CLEARTYPE_QUALITY;
    return CreateFontIndirectW(&lf);
}

unsigned long long Fnv1a64(const void* data, size_t len)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    unsigned long long hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++)
    {
        hash ^= p[i];
        hash *= 1099511628211ull;
    }
    return hash;
}
} /* namespace yz */
