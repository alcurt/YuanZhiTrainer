#include "exam.h"

#include "yz_hook_state.h"

#include <tlhelp32.h>

#include <string>

namespace
{
const wchar_t* const kExamModules[] =
{
    L"Exam.ads",
    L"ExamDlg.exe",
    L"ExamDlg.dll",
    L"ClassQuiz.ads",
    L"ORAL_EXAM.ocx",
    L"ExamEditor.exe"
};

const wchar_t* const kExamKeywords[] =
{
    L"考试",
    L"测验",
    L"答题",
    L"试卷",
    L"快问快答"
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

struct WindowSearch
{
    const wchar_t* keyword;
    bool           found;
};

BOOL CALLBACK ExamWindowProc(HWND hwnd, LPARAM lParam)
{
    WindowSearch* search = reinterpret_cast<WindowSearch*>(lParam);
    if (search->found)
        return FALSE;

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
    if (!yz::ContainsNoCase(path, L"YZinfo Multimedia teaching software") &&
        !yz::StartsWithNoCase(path, yzhook::g_targetDir))
        return TRUE;

    search->keyword = keyword;
    search->found   = true;
    return FALSE;
}
} /* namespace */

namespace yzhook
{
bool ExamDetect(const wchar_t** outReason)
{
    for (size_t i = 0; i < sizeof(kExamModules) / sizeof(kExamModules[0]); i++)
    {
        if (ModuleLoaded(kExamModules[i]))
        {
            if (outReason != nullptr)
                *outReason = kExamModules[i];
            return true;
        }
    }

    WindowSearch search;
    search.keyword = nullptr;
    search.found   = false;
    EnumWindows(ExamWindowProc, reinterpret_cast<LPARAM>(&search));
    if (search.found)
    {
        if (outReason != nullptr)
            *outReason = search.keyword;
        return true;
    }

    return false;
}
} /* namespace yzhook */
