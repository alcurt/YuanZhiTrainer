#include "exam.h"

#include "yz_hook_state.h"

#include <tlhelp32.h>

#include <stdio.h>
#include <string>
#include <vector>

namespace
{
/* 弱信号模块：学生端启动时就会随 Organs\ 目录一并加载，不能作为"正在考试"的充要条件 */
const wchar_t* const kWeakExamModules[] =
{
    L"Exam.ads",
    L"ClassQuiz.ads"
};

/* 强信号模块：只有真正进入考试/测验流程才会出现的交互组件 */
const wchar_t* const kStrongExamModules[] =
{
    L"ExamDlg.exe",
    L"ExamDlg.dll",
    L"ORAL_EXAM.ocx",
    L"ExamEditor.exe"
};

/* 强信号进程：考试对话框宿主进程 */
const wchar_t* const kStrongExamProcesses[] =
{
    L"ExamDlg.exe"
};

const wchar_t* const kExamKeywords[] =
{
    L"考试",
    L"测验",
    L"答题",
    L"试卷",
    L"快问快答",
    L"Exam",
    L"Quiz"
};

bool ModuleLoaded(const wchar_t* name)
{
    return GetModuleHandleW(name) != nullptr;
}

bool TitleMatched(const std::wstring& title, const wchar_t** outKeyword)
{
    for (size_t i = 0; i < sizeof(kExamKeywords) / sizeof(kExamKeywords[0]); i++)
    {
        if (title.find(kExamKeywords[i]) != std::wstring::npos)
        {
            if (outKeyword != nullptr)
                *outKeyword = kExamKeywords[i];
            return true;
        }
    }
    return false;
}

/* 目标安装目录的小写形式；g_targetDir 本身已是小写，这里只用于忽略大小写比较 */
const std::wstring& TargetDirLower()
{
    static std::wstring cached;
    static bool loaded = false;
    if (!loaded)
    {
        cached = yz::ToLower(yzhook::g_targetDir);
        loaded = true;
    }
    return cached;
}

/* 忽略大小写的目录前缀比较，避免 StartsWithNoCase 把 "…softwareV9.0 StudentX" 也算命中 */
bool PathWithin(const std::wstring& path, const std::wstring& base)
{
    if (path.empty() || base.empty())
        return false;
    if (path.size() < base.size())
        return false;
    if (yz::ToLower(path.substr(0, base.size())) != base)
        return false;
    if (path.size() == base.size())
        return true;
    wchar_t next = path[base.size()];
    return next == L'\\' || next == L'/';
}

std::wstring TruncateText(const std::wstring& in, size_t max, size_t tail)
{
    if (in.size() <= max)
        return in;
    std::wstring out = in.substr(0, max - tail);
    out += L"...";
    out += in.substr(in.size() - tail);
    return out;
}

/* 枚举目标进程当前模块，形如 "MainE.exe[1234] ..." */
void CollectModuleList(std::wstring* out)
{
    if (out == nullptr)
        return;
    out->clear();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return;

    MODULEENTRY32W entry;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snap, &entry))
    {
        do
        {
            std::wstring name = entry.szModule;
            if (name.size() > 64)
                name.resize(64);
            if (!out->empty())
                *out += L" ";
            out->append(name);
            out->append(yz::Format(L"[%u]", static_cast<unsigned>(entry.th32ProcessID)));
        } while (Module32NextW(snap, &entry));
    }
    CloseHandle(snap);
}

/* 按弱/强分组列出已加载模块及其完整路径 */
std::wstring LoadedModuleList(const wchar_t* const* names, size_t count, const wchar_t** outFirst)
{
    std::wstring list;
    if (outFirst != nullptr)
        *outFirst = nullptr;

    for (size_t i = 0; i < count; i++)
    {
        HMODULE mod = GetModuleHandleW(names[i]);
        if (mod == nullptr)
            continue;

        std::wstring path = yz::GetModuleFilePath(mod);
        if (!list.empty())
            list += L"; ";
        list += names[i];
        if (!path.empty())
        {
            list += L" (";
            list += path;
            list += L")";
        }

        if (outFirst != nullptr && *outFirst == nullptr)
            *outFirst = names[i];
    }
    return list;
}

size_t CountLoadedModules(const wchar_t* const* names, size_t count)
{
    size_t n = 0;
    for (size_t i = 0; i < count; i++)
    {
        if (ModuleLoaded(names[i]))
            n++;
    }
    return n;
}

/* 进程列表里是否存在指定的独立考试进程（弱信号模块常驻不算） */
bool ProcessRunning(const wchar_t* const* names, size_t count, std::wstring* outPath)
{
    if (outPath != nullptr)
        outPath->clear();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    bool found = false;
    PROCESSENTRY32W entry;
    ZeroMemory(&entry, sizeof(entry));
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry))
    {
        do
        {
            for (size_t i = 0; i < count; i++)
            {
                if (_wcsicmp(entry.szExeFile, names[i]) != 0)
                    continue;

                found = true;
                if (outPath != nullptr)
                {
                    std::wstring path = yz::GetProcessImagePath(entry.th32ProcessID);
                    if (!path.empty())
                        *outPath = path;
                }
                break;
            }
            if (found)
                break;
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

struct WindowSearch
{
    const wchar_t* keyword;
    std::wstring   title;
    std::wstring   path;
    bool           found;
};

BOOL CALLBACK ExamWindowProc(HWND hwnd, LPARAM lParam)
{
    WindowSearch* search = reinterpret_cast<WindowSearch*>(lParam);
    if (search->found)
        return FALSE;

    /* 只认可见窗口，避免隐藏/幽灵窗口造成误判 */
    if (!IsWindowVisible(hwnd))
        return TRUE;

    wchar_t title[512] = {0};
    if (GetWindowTextW(hwnd, title, 512) == 0)
        return TRUE;

    const wchar_t* keyword = nullptr;
    if (!TitleMatched(title, &keyword))
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0)
        return TRUE;

    std::wstring path = yz::GetProcessImagePath(pid);
    if (path.empty())
        return TRUE;
    if (!yz::IsYuanzhiInstallPath(path) &&
        !PathWithin(path, TargetDirLower()))
        return TRUE;

    search->keyword = keyword;
    search->title   = title;
    search->path    = path;
    search->found   = true;
    return FALSE;
}

bool DetectStrong(yzhook::ExamDetail* out)
{
    /* 强信号 1：独立考试进程 ExamDlg.exe 正在运行 */
    std::wstring procPath;
    if (ProcessRunning(kStrongExamProcesses,
                       sizeof(kStrongExamProcesses) / sizeof(kStrongExamProcesses[0]),
                       &procPath))
    {
        out->reasonKind  = yzhook::kExamReasonProcess;
        out->reason      = kStrongExamProcesses[0];
        out->windowPath  = procPath;
        out->moduleName  = LoadedModuleList(kStrongExamModules,
                                            sizeof(kStrongExamModules) / sizeof(kStrongExamModules[0]),
                                            nullptr);
        return true;
    }

    /* 强信号 2：强信号交互组件已加载（考试对话框模块） */
    const wchar_t* firstStrong = nullptr;
    std::wstring strong = LoadedModuleList(kStrongExamModules,
                                           sizeof(kStrongExamModules) / sizeof(kStrongExamModules[0]),
                                           &firstStrong);
    if (firstStrong != nullptr)
    {
        out->reasonKind = yzhook::kExamReasonModule;
        out->reason     = firstStrong;
        out->moduleName = strong;
        return true;
    }

    /* 强信号 3：可见窗口标题命中考试关键词，且窗口属于远志安装目录 */
    WindowSearch search;
    search.keyword = nullptr;
    search.found   = false;
    EnumWindows(ExamWindowProc, reinterpret_cast<LPARAM>(&search));
    if (search.found)
    {
        out->reasonKind  = yzhook::kExamReasonWindow;
        out->reason      = search.keyword;
        out->windowTitle = search.title;
        out->windowPath  = search.path;
        return true;
    }

    return false;
}

bool DetectWeak(yzhook::ExamDetail* out, size_t* outCount)
{
    const wchar_t* firstWeak = nullptr;
    std::wstring weak = LoadedModuleList(kWeakExamModules,
                                         sizeof(kWeakExamModules) / sizeof(kWeakExamModules[0]),
                                         &firstWeak);
    if (out != nullptr)
    {
        if (firstWeak != nullptr)
        {
            out->reasonKind = yzhook::kExamReasonWeakModule;
            out->reason     = firstWeak;
            out->moduleName = weak;
        }
        else
        {
            out->reasonKind = yzhook::kExamReasonNone;
            out->reason     = nullptr;
            out->moduleName.clear();
            out->windowTitle.clear();
            out->windowPath.clear();
        }
    }

    size_t count = CountLoadedModules(kWeakExamModules,
                                      sizeof(kWeakExamModules) / sizeof(kWeakExamModules[0]));
    if (outCount != nullptr)
        *outCount = count;
    return count != 0;
}
} /* namespace */

namespace yzhook
{
bool ExamDetect(const wchar_t** outReason)
{
    ExamDetail detail;
    bool exam = ExamDetect(&detail);
    if (outReason != nullptr)
        *outReason = exam ? detail.reason : nullptr;
    return exam;
}

bool ExamDetect(ExamDetail* outDetail)
{
    ExamDetail local;
    ExamDetail& d = (outDetail != nullptr) ? *outDetail : local;

    d.reasonKind = kExamReasonNone;
    d.reason     = nullptr;
    d.moduleName.clear();
    d.windowTitle.clear();
    d.windowPath.clear();

    return DetectStrong(&d);
}

bool ExamDetectWeak(ExamDetail* outDetail, size_t* outCount)
{
    return DetectWeak(outDetail, outCount);
}

std::wstring ExamLoadedModuleList()
{
    const wchar_t* first = nullptr;
    return LoadedModuleList(kWeakExamModules,
                            sizeof(kWeakExamModules) / sizeof(kWeakExamModules[0]),
                            &first);
}

std::wstring ExamStrongModuleList()
{
    const wchar_t* first = nullptr;
    return LoadedModuleList(kStrongExamModules,
                            sizeof(kStrongExamModules) / sizeof(kStrongExamModules[0]),
                            &first);
}

std::wstring ModuleListDigest(size_t maxItems)
{
    std::wstring list;
    CollectModuleList(&list);
    if (list.empty())
        return L"(枚举失败)";

    if (maxItems != 0)
    {
        std::vector<std::wstring> items;
        size_t start = 0;
        for (;;)
        {
            size_t sp = list.find(L' ', start);
            if (sp == std::wstring::npos)
            {
                items.push_back(list.substr(start));
                break;
            }
            items.push_back(list.substr(start, sp - start));
            start = sp + 1;
        }

        if (items.size() > maxItems)
        {
            std::wstring out;
            for (size_t i = 0; i < maxItems; i++)
            {
                if (!out.empty())
                    out += L" ";
                out += items[i];
            }
            out += yz::Format(L" ...(另有 %u 个)", static_cast<unsigned>(items.size() - maxItems));
            return out;
        }
    }

    return list;
}

std::wstring ExamDetailText(const ExamDetail& detail, std::wstring* cache)
{
    std::wstring out = yz::Format(L"考试模式诊断: 命中原因=%s",
                                  (detail.reasonKind == kExamReasonProcess) ? L"独立考试进程" :
                                  (detail.reasonKind == kExamReasonModule) ? L"强信号模块已加载" :
                                  (detail.reasonKind == kExamReasonWindow) ? L"可见窗口标题命中" :
                                  (detail.reasonKind == kExamReasonWeakModule) ? L"弱信号模块已加载(不熔断)" :
                                  L"未命中");

    if (detail.reasonKind == kExamReasonProcess)
    {
        out += yz::Format(L"\r\n考试进程: %s", (detail.reason != nullptr) ? detail.reason : L"未知");
        out += L"\r\n进程路径: ";
        out += detail.windowPath.empty() ? L"(未知)" : TruncateText(detail.windowPath, 220, 60);
    }
    else if (detail.reasonKind == kExamReasonModule)
    {
        out += L"\r\n强信号模块: ";
        out += detail.moduleName.empty() ? L"(无)" : TruncateText(detail.moduleName, 300, 60);
    }
    else if (detail.reasonKind == kExamReasonWindow)
    {
        out += yz::Format(L"\r\n命中关键词: %s", (detail.reason != nullptr) ? detail.reason : L"未知");
        out += L"\r\n窗口标题: ";
        out += detail.windowTitle.empty() ? L"(空)" : TruncateText(detail.windowTitle, 220, 60);
        out += L"\r\n窗口进程: ";
        out += detail.windowPath.empty() ? L"(未知)" : TruncateText(detail.windowPath, 220, 60);
    }
    else if (detail.reasonKind == kExamReasonWeakModule)
    {
        out += L"\r\n弱信号模块: ";
        out += detail.moduleName.empty() ? L"(无)" : TruncateText(detail.moduleName, 300, 60);
    }

    std::wstring strong = ExamStrongModuleList();
    out += L"\r\n强信号模块: ";
    out += strong.empty() ? L"(无)" : TruncateText(strong, 200, 60);

    out += L"\r\n进程模块: ";
    out += ModuleListDigest(16);

    if (cache != nullptr)
        *cache = out;
    return out;
}

std::wstring ExamDetailText(const ExamDetail& detail)
{
    return ExamDetailText(detail, nullptr);
}
} /* namespace yzhook */
