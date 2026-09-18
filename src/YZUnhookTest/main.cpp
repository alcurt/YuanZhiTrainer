//
// YZUnhookTest.exe —— “免注入拔钩”验证工具（分支 A）
//
// 目的：验证一条不注入目标进程的解锁路线——只在**本进程**里加载远志自己的
//       KeyboardHook.dll / ExdHooks.dll，然后调用它们的导出 KillHook /
//       UnSetExdHooks / UnSetExdHooks2。依据是这两个 DLL 的钩子句柄放在
//       **跨进程共享节**（HookData / .ExdHook）里，而 HHOOK 是会话级对象，
//       因此本进程调用同一份代码，撤销的是同一个钩子集合。
//
// 安全设计：
//   * 默认只“体检”（读文件导出表 + 指令流校验），不加载、不调用；--apply 才动手。
//   * 调用前对代码做调用约定自检（HDE32，与 YZHook 内同一份源码）。
//   * 每个调用放在独立线程上并限时等待（KillHook 内部有 INFINITE 等待），调用点 SEH 保护。
//   * 不 FreeLibrary：调用完直接退出进程，避开 DLL 卸载路径的未知副作用。
//
// 用法：
//   YZUnhookTest.exe [--install-dir <远志安装目录>] [--apply] [--wait <毫秒>]
//   不给 --install-dir 时会尝试从正在运行的 Yistart.exe / Nmdeputy.exe 推断。
//
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>

#include <string.h>
#include <locale.h>
#include <string>
#include <tlhelp32.h>
#include <vector>

#include "signature_check.h"
#include "yz_protocol.h"
#include "yz_util.h"

namespace
{
/* ---------- 输出：同时写控制台与同目录报告文件 ---------- */
HANDLE g_log     = INVALID_HANDLE_VALUE;
bool   g_noPause = false;
bool   g_arm     = false;   /* --arm：先待机，回到窗口按回车再执行 */
int    g_delay   = 0;       /* --delay N：N 秒后自动执行（键盘被锁时用） */

void Out(const wchar_t* fmt, ...)
{
    wchar_t buf[4096] = {0};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, ARRAYSIZE(buf), _TRUNCATE, fmt, args);
    va_end(args);

    /* 控制台走 WriteConsoleW：避免重定向/代码页把中文变成 '?' */
    HANDLE out  = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    if (out != nullptr && out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode))
    {
        DWORD written = 0;
        WriteConsoleW(out, buf, static_cast<DWORD>(wcslen(buf)), &written, nullptr);
    }
    else if (out != nullptr && out != INVALID_HANDLE_VALUE)
    {
        char utf8[8192] = {0};
        const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        DWORD written = 0;
        if (n > 0)
            WriteFile(out, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
    }

    if (g_log != INVALID_HANDLE_VALUE)
    {
        char utf8[8192] = {0};
        const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
        DWORD written = 0;
        if (n > 0)
            WriteFile(g_log, utf8, static_cast<DWORD>(n - 1), &written, nullptr);
    }
}

/* 统一退出路径：双击运行时停等回车，别让窗口一闪而过；同时关掉报告文件 */
int Finish(int code)
{
    if (!g_noPause)
        yz::PauseIfSoleConsole();
    if (g_log != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
    return code;
}

struct TargetEntry
{
    const wchar_t* relPath;
    const char*    proc;
    bool           takesArg;
    bool           argSelfTid;
    const wchar_t* label;
};

const TargetEntry kTargets[] =
{
    { L"Organs\\KeyboardHook.dll", "KillHook",       false, false, L"KeyboardHook!KillHook()" },
    { L"KsFiles\\ExdHooks.dll",    "UnSetExdHooks",  true,  false, L"ExdHooks!UnSetExdHooks(0)" },
    { L"KsFiles\\ExdHooks.dll",    "UnSetExdHooks2", true,  true,  L"ExdHooks!UnSetExdHooks2(本线程tid)" },
};

/* 共享节里“钩子句柄/状态字”的 RVA：只用于调用前后对照，来自本机 V9.0 Student 静态分析。
   目标机版本不同只会让对照值失去意义，不影响调用本身。 */
struct SnapshotSpec
{
    const wchar_t* relPath;
    const wchar_t* note;
    DWORD          rvas[6];
    int            count;
};

const SnapshotSpec kSnapshots[] =
{
    { L"Organs\\KeyboardHook.dll", L"HookData: 句柄0/1/2 + 标志字",
      { 0x23302C, 0x233030, 0x233034, 0x233148, 0, 0 }, 4 },
    { L"KsFiles\\ExdHooks.dll", L".ExdHook: 句柄/归属/状态字",
      { 0x700000, 0x700004, 0x700008, 0x70000C, 0x700010, 0 }, 5 },
};

/* ---------- 只读 PE 解析：体检阶段拿导出 RVA 与代码字节 ---------- */
struct PeFile
{
    struct Section { DWORD va; DWORD vsize; DWORD raw; DWORD rawsize; };
    std::vector<BYTE>    bytes;
    std::vector<Section> sections;
    DWORD                exportRva = 0;
};

bool LoadPeFile(const std::wstring& path, PeFile* out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size;
    size.QuadPart = 0;
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > (64ll << 20))
    {
        CloseHandle(h);
        return false;
    }

    out->bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    const bool ok = ReadFile(h, out->bytes.data(), static_cast<DWORD>(out->bytes.size()), &got, nullptr) != FALSE &&
                    got == out->bytes.size();
    CloseHandle(h);
    if (!ok)
        return false;

    const BYTE* b = out->bytes.data();
    if (out->bytes.size() < 64)
        return false;
    const DWORD lfanew = *reinterpret_cast<const DWORD*>(b + 0x3C);
    if (lfanew + 24 > out->bytes.size())
        return false;

    const WORD magic   = *reinterpret_cast<const WORD*>(b + lfanew + 24);
    const WORD numSec  = *reinterpret_cast<const WORD*>(b + lfanew + 6);
    const WORD optSize = *reinterpret_cast<const WORD*>(b + lfanew + 20);
    const DWORD optOff = lfanew + 24;
    const DWORD ddOff  = (magic == 0x20B) ? optOff + 112 : optOff + 96;
    if (ddOff + 8 > out->bytes.size())
        return false;
    out->exportRva = *reinterpret_cast<const DWORD*>(b + ddOff);

    const DWORD secOff = optOff + optSize;
    for (WORD i = 0; i < numSec; i++)
    {
        const DWORD o = secOff + static_cast<DWORD>(i) * 40;
        if (o + 40 > out->bytes.size())
            break;
        PeFile::Section s;
        s.vsize   = *reinterpret_cast<const DWORD*>(b + o + 8);
        s.va      = *reinterpret_cast<const DWORD*>(b + o + 12);
        s.rawsize = *reinterpret_cast<const DWORD*>(b + o + 16);
        s.raw     = *reinterpret_cast<const DWORD*>(b + o + 20);
        out->sections.push_back(s);
    }
    return true;
}

DWORD PeRvaToOffset(const PeFile& pe, DWORD rva)
{
    for (size_t i = 0; i < pe.sections.size(); i++)
    {
        const PeFile::Section& s = pe.sections[i];
        const DWORD span = (s.vsize > s.rawsize) ? s.vsize : s.rawsize;
        if (rva >= s.va && rva < s.va + span)
        {
            const DWORD off = s.raw + (rva - s.va);
            return (off < pe.bytes.size()) ? off : 0;
        }
    }
    return 0;
}

bool PeFindExport(const PeFile& pe, const char* name, DWORD* outRva)
{
    if (pe.exportRva == 0)
        return false;

    const DWORD off = PeRvaToOffset(pe, pe.exportRva);
    if (off == 0 || off + sizeof(IMAGE_EXPORT_DIRECTORY) > pe.bytes.size())
        return false;

    const IMAGE_EXPORT_DIRECTORY* dir =
        reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(pe.bytes.data() + off);
    const DWORD namesOff = PeRvaToOffset(pe, dir->AddressOfNames);
    const DWORD ordsOff  = PeRvaToOffset(pe, dir->AddressOfNameOrdinals);
    const DWORD funcsOff = PeRvaToOffset(pe, dir->AddressOfFunctions);
    if (namesOff == 0 || ordsOff == 0 || funcsOff == 0)
        return false;

    for (DWORD i = 0; i < dir->NumberOfNames; i++)
    {
        const DWORD nameRva = *reinterpret_cast<const DWORD*>(pe.bytes.data() + namesOff + i * 4);
        const DWORD nameOff = PeRvaToOffset(pe, nameRva);
        if (nameOff == 0)
            continue;
        if (strcmp(reinterpret_cast<const char*>(pe.bytes.data() + nameOff), name) != 0)
            continue;
        const WORD ord = *reinterpret_cast<const WORD*>(pe.bytes.data() + ordsOff + i * 2);
        *outRva = *reinterpret_cast<const DWORD*>(pe.bytes.data() + funcsOff + ord * 4);
        return true;
    }
    return false;
}

/* ---------- 调用（限时 + SEH） ---------- */
struct CallArgs
{
    void*  proc;
    DWORD  arg;
    bool   takesArg;
    DWORD  result;
    DWORD  seh;
    bool   ok;
};

DWORD WINAPI CallEntry(LPVOID lp)
{
    CallArgs* a = reinterpret_cast<CallArgs*>(lp);
    if (a->takesArg)
    {
        typedef DWORD (__cdecl *PFN1)(DWORD);
        __try
        {
            a->result = reinterpret_cast<PFN1>(a->proc)(a->arg);
            a->ok     = true;
        }
        __except (a->seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            a->ok = false;
        }
    }
    else
    {
        typedef void (__cdecl *PFN0)(void);
        __try
        {
            reinterpret_cast<PFN0>(a->proc)();
            a->ok = true;
        }
        __except (a->seh = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
        {
            a->ok = false;
        }
    }
    return 0;
}

/* 参数放在堆上：超时就故意不回收（线程可能永远卡在对方代码里，进程随后退出） */
CallArgs* CallTimed(void* proc, bool takesArg, DWORD arg, DWORD waitMs, bool* completed)
{
    CallArgs* a = new CallArgs();
    a->proc     = proc;
    a->arg      = arg;
    a->takesArg = takesArg;
    a->result   = 0;
    a->seh      = 0;
    a->ok       = false;

    HANDLE th = CreateThread(nullptr, 0, CallEntry, a, 0, nullptr);
    if (th == nullptr)
    {
        *completed = false;
        return a;
    }
    const DWORD w = WaitForSingleObject(th, waitMs);
    CloseHandle(th);
    *completed = (w == WAIT_OBJECT_0);
    return a;
}

bool ReadGlobals(HMODULE mod, const DWORD* rvas, int count, DWORD* out)
{
    __try
    {
        for (int i = 0; i < count; i++)
            out[i] = *reinterpret_cast<const DWORD*>(reinterpret_cast<const BYTE*>(mod) + rvas[i]);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

/* ---------- 安装目录推断：找正在运行的远志进程 ---------- */
std::wstring FindInstallDir()
{
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return std::wstring();

    static const wchar_t* const kNames[] =
    {
        L"Yistart.exe", L"Nmdeputy.exe", L"TEACHCMD.exe", L"PlayerGUI.exe",
        L"ExdPaintHelper.exe", L"YZTrainer.exe"
    };

    std::wstring dir;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe))
    {
        do
        {
            for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); i++)
            {
                if (_wcsicmp(pe.szExeFile, kNames[i]) != 0)
                    continue;
                const std::wstring path = yz::GetProcessImagePath(pe.th32ProcessID);
                if (!path.empty() &&
                    (yz::ContainsNoCase(path, L"YZinfo Multimedia teaching software") ||
                     yz::ContainsNoCase(path, L"GZYZ")))
                {
                    dir = yz::DirNameOf(path);
                }
                break;
            }
            if (!dir.empty())
                break;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return dir;
}

std::wstring FormatDwords(const DWORD* v, int count)
{
    std::wstring s;
    for (int i = 0; i < count; i++)
        s += yz::Format(L"%s0x%08X", (i == 0) ? L"" : L" ", v[i]);
    return s;
}
} /* namespace */

int wmain(int argc, wchar_t** argv)
{
    /* 控制台按 UTF-8 输出，CRT 也要用 UTF-8 区域设置，否则宽字符会退化成 '?' */
    SetConsoleOutputCP(CP_UTF8);
    setlocale(LC_ALL, ".UTF8");

    /* 报告文件：双击运行时控制台会随进程一起消失，所以同一份输出也落盘 */
    const std::wstring logPath =
        yz::JoinPath(yz::GetExeDir(), L"YZUnhookTest-" + yz::NowStamp() + L".txt");
    g_log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::wstring installDir;
    bool         apply  = false;
    DWORD        waitMs = 3000;

    for (int i = 1; i < argc; i++)
    {
        const std::wstring arg = argv[i];
        if ((arg == L"--install-dir" || arg == L"-d") && i + 1 < argc)
            installDir = argv[++i];
        else if (arg == L"--apply")
            apply = true;
        else if (arg == L"--arm")
        {
            apply = true;
            g_arm = true;
        }
        else if (arg == L"--delay" && i + 1 < argc)
        {
            apply = true;
            g_delay = _wtoi(argv[++i]);
            if (g_delay < 0)
                g_delay = 0;
        }
        else if ((arg == L"--wait" || arg == L"-w") && i + 1 < argc)
            waitMs = static_cast<DWORD>(_wtoi(argv[++i]));
        else if (arg == L"--no-pause")
            g_noPause = true;
        else if (arg == L"-h" || arg == L"--help")
        {
            Out(L"用法: YZUnhookTest.exe [--install-dir <远志安装目录>] [--apply] [--arm] [--delay <秒>] [--wait <毫秒>] [--no-pause]\n");
            Out(L"  默认只体检：读文件导出表 + 调用约定自检，不加载、不调用\n");
            Out(L"  --apply 才真正 LoadLibrary 并调用 KillHook / UnSetExdHooks / UnSetExdHooks2\n");
            Out(L"  --arm      先体检并待机，回到本窗口按回车后立刻执行（“先武装、再去被锁屏/广播”）\n");
            Out(L"  --delay N  待机 N 秒后自动执行（键盘被锁、敲不了回车时用这个）\n");
            Out(L"  --wait N   单个导出调用的等待上限毫秒数（默认 3000）\n");
            Out(L"  双击运行时结束会等你按回车；脚本里用 --no-pause 跳过\n");
            return Finish(0);
        }
    }

    Out(L"YZUnhookTest %s —— 免注入拔钩验证工具\n", YZ_VERSION_STR);
    if (installDir.empty())
        installDir = FindInstallDir();
    if (installDir.empty())
    {
        Out(L"找不到远志安装目录：请用 --install-dir 指定（例如 C:\\Program Files (x86)\\GZYZ\\YZinfo Multimedia teaching softwareV9.0 Student）\n");
        return Finish(2);
    }
    Out(L"远志安装目录: %ls\n", installDir.c_str());
    Out(L"探针位数: %s\n\n", (sizeof(void*) == 8) ? L"x64" : L"x86");

    /* ---------- 1) 体检 ---------- */
    Out(L"=== 1) 体检（只读文件，不加载任何远志 DLL）===\n");
    int planOk = 0;
    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); i++)
    {
        const TargetEntry& t = kTargets[i];
        const std::wstring dllPath = yz::JoinPath(installDir, t.relPath);

        PeFile pe;
        if (!LoadPeFile(dllPath, &pe))
        {
            Out(L"  [跳过] %-42ls 打不开文件\n", t.label);
            continue;
        }

        DWORD rva = 0;
        if (!PeFindExport(pe, t.proc, &rva))
        {
            Out(L"  [无导出] %-40ls %ls 里找不到 %S\n", t.label, t.relPath, t.proc);
            continue;
        }

        const DWORD codeOff = PeRvaToOffset(pe, rva);
        if (codeOff == 0)
        {
            Out(L"  [异常] %-42ls 导出 RVA 0x%X 无法映射\n", t.label, rva);
            continue;
        }

        SIZE_T      avail  = pe.bytes.size() - codeOff;
        const SIZE_T scanLen = (avail < 384) ? avail : 384;
        wchar_t     why[192] = {0};
        const bool  sigOk = yzhook::SignatureLooksCallable(pe.bytes.data() + codeOff, scanLen,
                                                           t.takesArg, why, ARRAYSIZE(why));
        Out(L"  [%ls] %-40ls RVA=0x%06X 调用约定自检: %ls %ls\n",
                sigOk ? L"可调用" : L"拒绝", t.label, rva,
                sigOk ? L"通过" : L"失败", sigOk ? L"" : why);
        if (sigOk)
            planOk++;
    }

    if (planOk == 0)
    {
        Out(L"\n没有任何入口通过体检，放弃。\n");
        return Finish(3);
    }
    if (!apply)
    {
        Out(L"\n体检完成（%d 个入口可调用）。这只是演练：真正调用请加 --apply。\n", planOk);
        Out(L"报告文件: %ls\n", logPath.c_str());
        return Finish(0);
    }

    /* ---------- 2) 实际调用 ---------- */
    Out(L"\n=== 2) 实际调用（--apply）===\n");
    Out(L"注意：这会真的撤销全局钩子（目标就是如此），且不卸载已加载的 DLL。\n\n");

    /* 先武装、后触发：被锁屏/锁键鼠时你自己没法再开程序、也敲不了键，
       所以这两步必须在还能操作的时候就做。
       --delay N 到点自动执行（键盘被锁时唯一可行的触发方式）；
       --arm 则等你回到本窗口按回车。 */
    if (g_delay > 0)
    {
        Out(L"\n%d 秒后自动执行拔钩。现在切到被锁屏/广播的场景，不用再管这个窗口。\n", g_delay);
        for (int left = g_delay; left > 0; left--)
        {
            Out(L"  倒计时 %d 秒…\n", left);
            Sleep(1000);
        }
        Out(L"  时间到，开始执行。\n");
    }
    else if (g_arm)
    {
        Out(L"\n已就绪。现在切到被锁屏/广播的场景，然后回到本窗口按回车立即执行。\n");
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        DWORD  mode = 0;
        if (in != nullptr && in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode))
        {
            char buf[8];
            DWORD got = 0;
            ReadFile(in, buf, sizeof(buf), &got, nullptr);
        }
        else
        {
            Out(L"（当前输入不是控制台，无法等待回车，直接执行）\n");
        }
        Out(L"  开始执行。\n");
    }

    struct LoadedDll { std::wstring relPath; HMODULE mod; };
    std::vector<LoadedDll> loaded;

    int called = 0;
    for (size_t i = 0; i < sizeof(kTargets) / sizeof(kTargets[0]); i++)
    {
        const TargetEntry& t = kTargets[i];
        const std::wstring dllPath = yz::JoinPath(installDir, t.relPath);

        HMODULE mod = nullptr;
        for (size_t k = 0; k < loaded.size(); k++)
        {
            if (loaded[k].relPath == t.relPath)
            {
                mod = loaded[k].mod;
                break;
            }
        }
        if (mod == nullptr)
        {
            mod = LoadLibraryW(dllPath.c_str());
            if (mod == nullptr)
            {
                Out(L"  [失败] 加载 %ls 失败 err=0x%08X\n", t.relPath, GetLastError());
                continue;
            }
            LoadedDll ld;
            ld.relPath = t.relPath;
            ld.mod     = mod;
            loaded.push_back(ld);
        }

        void* proc = reinterpret_cast<void*>(GetProcAddress(mod, t.proc));
        if (proc == nullptr)
        {
            Out(L"  [失败] %ls 里 GetProcAddress(%S) 为空\n", t.relPath, t.proc);
            continue;
        }

        wchar_t    why[192] = {0};
        const bool sigOk = yzhook::SignatureLooksCallable(reinterpret_cast<const unsigned char*>(proc),
                                                          384, t.takesArg, why, ARRAYSIZE(why));
        if (!sigOk)
        {
            Out(L"  [拒绝] %-40ls 运行时自检未过：%ls\n", t.label, why);
            continue;
        }

        /* 调用前后读一次共享节里的句柄/状态字（仅对照，读失败不影响调用） */
        DWORD before[6] = {0};
        DWORD after[6]  = {0};
        bool  haveSnap  = false;
        for (size_t s = 0; s < sizeof(kSnapshots) / sizeof(kSnapshots[0]); s++)
        {
            if (_wcsicmp(kSnapshots[s].relPath, t.relPath) == 0)
            {
                haveSnap = ReadGlobals(mod, kSnapshots[s].rvas, kSnapshots[s].count, before);
                break;
            }
        }

        const DWORD arg = t.argSelfTid ? GetCurrentThreadId() : 0;
        bool        completed = false;
        CallArgs*   a = CallTimed(proc, t.takesArg, arg, waitMs, &completed);

        if (!completed)
        {
            Out(L"  [超时] %-40ls %u ms 内没返回（线程留在对方代码里，不再回收）\n", t.label, waitMs);
        }
        else if (!a->ok)
        {
            Out(L"  [异常] %-40ls 触发 0x%08X\n", t.label, a->seh);
        }
        else
        {
            called++;
            if (t.takesArg)
                Out(L"  [已调用] %-38ls 返回 %u\n", t.label, a->result);
            else
                Out(L"  [已调用] %-38ls (无返回值)\n", t.label);
        }

        if (haveSnap)
        {
            for (size_t s = 0; s < sizeof(kSnapshots) / sizeof(kSnapshots[0]); s++)
            {
                if (_wcsicmp(kSnapshots[s].relPath, t.relPath) != 0)
                    continue;
                const bool okAfter = ReadGlobals(mod, kSnapshots[s].rvas, kSnapshots[s].count, after);
                Out(L"           对照 %ls\n", kSnapshots[s].note);
                Out(L"             调用前: %ls\n", FormatDwords(before, kSnapshots[s].count).c_str());
                Out(L"             调用后: %ls%ls\n", okAfter ? L"" : L"(读取失败) ",
                        FormatDwords(after, kSnapshots[s].count).c_str());
                break;
            }
        }
    }

    Out(L"\n完成：成功调用 %d 个入口。请同时观察键鼠是否恢复（这才是最终判据）。\n", called);
    Out(L"本进程不卸载已加载的远志 DLL，直接退出即可。\n");
    Out(L"报告文件: %ls\n", logPath.c_str());
    return Finish((called > 0) ? 0 : 4);
}
