//
// 内嵌 Hook 载荷：资源 → 架构校验 → 释放到磁盘。
//
// 设计要点：
//   1. 只释放与本进程架构一致的 DLL（读 PE FileHeader.Machine）。构建脚本
//      配对出错时，宁可在这里明确失败，也不要让目标进程的 LoadLibraryW
//      返回一个没有下文的 0。
//   2. 文件名带内容哈希，DLL 一变文件名就变，不会出现"旧版本被复用"。
//   3. %ProgramData% 优先，失败退 %TEMP%。目标进程由服务在会话 1 拉起，
//      不保证与本程序同用户配置，而 ProgramData 对两个会话都稳定可见。
//   4. 释放出来的文件保留不删：命名带哈希，残留文件既不会用错版本，
//      也便于事后排障（这是与用户确认过的取舍）。
//
#include "payload.h"

#include "resource.h"

#include "yz_log.h"
#include "yz_util.h"

#include <string.h>
#include <vector>

namespace
{
#ifdef _WIN64
const WORD         kSelfMachine  = IMAGE_FILE_MACHINE_AMD64;
const wchar_t*     kSelfArchName = L"x64";
#else
const WORD         kSelfMachine  = IMAGE_FILE_MACHINE_I386;
const wchar_t*     kSelfArchName = L"x86";
#endif

struct Blob
{
    const BYTE* data;
    DWORD       size;
};

bool LoadEmbeddedBlob(Blob* out)
{
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    HRSRC     res   = FindResourceW(hinst, MAKEINTRESOURCEW(IDR_HOOK_DLL), RT_RCDATA);
    if (res == nullptr)
        return false;

    DWORD   size   = SizeofResource(hinst, res);
    HGLOBAL handle = LoadResource(hinst, res);
    if (handle == nullptr || size == 0)
        return false;

    const void* data = LockResource(handle);
    if (data == nullptr)
        return false;

    out->data = static_cast<const BYTE*>(data);
    out->size = size;
    return true;
}

/* 只读 PE 头里的 Machine 字段，不做完整合法性校验（那是加载器的事）。 */
bool PeFileMachine(const BYTE* data, DWORD size, WORD* machine)
{
    if (size < sizeof(IMAGE_DOS_HEADER))
        return false;

    IMAGE_DOS_HEADER dos;
    memcpy(&dos, data, sizeof(dos));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
        return false;

    const DWORD ntOff = static_cast<DWORD>(dos.e_lfanew);
    if (ntOff + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > size)
        return false;

    DWORD signature = 0;
    memcpy(&signature, data + ntOff, sizeof(signature));
    if (signature != IMAGE_NT_SIGNATURE)
        return false;

    IMAGE_FILE_HEADER fh;
    memcpy(&fh, data + ntOff + sizeof(DWORD), sizeof(fh));
    *machine = fh.Machine;
    return true;
}

bool FileHasContent(const std::wstring& path, const BYTE* data, DWORD size)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    bool         ok = false;
    LARGE_INTEGER li;
    li.QuadPart = 0;
    if (GetFileSizeEx(h, &li) && li.QuadPart == static_cast<LONGLONG>(size))
    {
        std::vector<BYTE> buf(size);
        DWORD             got = 0;
        if (ReadFile(h, buf.data(), size, &got, nullptr) && got == size)
            ok = (memcmp(buf.data(), data, size) == 0);
    }
    CloseHandle(h);
    return ok;
}

bool WriteAll(const std::wstring& path, const BYTE* data, DWORD size, std::wstring* err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
    {
        if (err != nullptr)
            *err = yz::Format(L"创建文件失败 (err=%u)", GetLastError());
        return false;
    }

    DWORD written = 0;
    bool  ok      = WriteFile(h, data, size, &written, nullptr) != FALSE && written == size;
    if (!ok)
    {
        if (err != nullptr)
            *err = yz::Format(L"写入失败 (err=%u)", GetLastError());
    }
    else if (!FlushFileBuffers(h))
    {
        ok = false;
        if (err != nullptr)
            *err = yz::Format(L"刷盘失败 (err=%u)", GetLastError());
    }
    CloseHandle(h);

    /* 写入不完整时不删半成品：文件名带内容哈希，且每次复用前都会整文件比对，
       半成品不可能被当成有效载荷使用，下次运行会被 CREATE_ALWAYS 覆盖。 */
    return ok;
}

/* 递归建目录：yz::EnsureDirectory 只做单层 CreateDirectoryW，而
   %ProgramData%\YZTrainer\cache 有两级，直接建会失败并静默退到 %TEMP%。 */
bool EnsureDirTree(const std::wstring& dir)
{
    if (dir.empty())
        return false;
    if (yz::EnsureDirectory(dir))
        return true;

    const size_t pos = dir.find_last_of(L"\\/");
    if (pos == std::wstring::npos || pos == 0)
        return false;
    if (!EnsureDirTree(dir.substr(0, pos)))
        return false;
    return yz::EnsureDirectory(dir);
}

void CollectCandidateDirs(std::vector<std::wstring>& out)
{
    wchar_t buf[1024] = {0};  /* 环境变量比这还长就退到 %TEMP% */
    DWORD   n          = GetEnvironmentVariableW(L"ProgramData", buf, ARRAYSIZE(buf));
    if (n > 0 && n < ARRAYSIZE(buf))
        out.push_back(yz::JoinPath(std::wstring(buf, n), L"YZTrainer\\cache"));

    std::wstring tmp = yz::GetTempDir();
    if (!tmp.empty())
        out.push_back(yz::JoinPath(tmp, L"YZTrainer-cache"));
}
} /* namespace */

std::wstring PayloadEnsureHookDll(std::wstring* err)
{
    if (err != nullptr)
        err->clear();

    Blob blob;
    if (!LoadEmbeddedBlob(&blob))
    {
        if (err != nullptr)
            *err = L"本程序没有内嵌 Hook 资源（可能不是通过本工程构建的 YZTrainer.exe）";
        return std::wstring();
    }

    WORD machine = 0;
    if (!PeFileMachine(blob.data, blob.size, &machine))
    {
        if (err != nullptr)
            *err = L"内嵌资源不是有效的 PE 文件";
        return std::wstring();
    }
    if (machine != kSelfMachine)
    {
        if (err != nullptr)
            *err = yz::Format(L"内嵌 Hook 架构不匹配：资源 Machine=0x%04X，本进程需要 0x%04X",
                              machine, kSelfMachine);
        return std::wstring();
    }

    const unsigned long long hash = yz::Fnv1a64(blob.data, blob.size);
    const std::wstring name = yz::Format(L"YZHook_%s_%08X%08X.dll", kSelfArchName,
                                         static_cast<unsigned>(hash >> 32),
                                         static_cast<unsigned>(hash & 0xFFFFFFFFull));

    std::vector<std::wstring> dirs;
    CollectCandidateDirs(dirs);

    std::wstring lastErr;
    for (size_t i = 0; i < dirs.size(); i++)
    {
        const std::wstring path = yz::JoinPath(dirs[i], name);
        if (FileHasContent(path, blob.data, blob.size))
        {
            YZLOGD(L"复用已释放的内嵌 Hook: %s", path.c_str());
            return path;
        }

        if (!EnsureDirTree(dirs[i]))
        {
            lastErr = yz::Format(L"%s: 无法创建目录 (err=%u)", dirs[i].c_str(), GetLastError());
            continue;
        }

        std::wstring werr;
        if (!WriteAll(path, blob.data, blob.size, &werr))
        {
            lastErr = yz::Format(L"%s: %s", dirs[i].c_str(), werr.c_str());
            continue;
        }

        YZLOGI(L"内嵌 Hook 已释放: %s (%u 字节, hash=%016llX)", path.c_str(), blob.size, hash);
        return path;
    }

    if (err != nullptr)
        *err = lastErr.empty() ? L"找不到可写的释放目录" : lastErr;
    return std::wstring();
}

DWORD PayloadEmbeddedSize()
{
    Blob blob;
    return LoadEmbeddedBlob(&blob) ? blob.size : 0;
}

bool PayloadCheckEmbedded()
{
    Blob blob;
    WORD machine = 0;
    return LoadEmbeddedBlob(&blob) && PeFileMachine(blob.data, blob.size, &machine) &&
           machine == kSelfMachine;
}
