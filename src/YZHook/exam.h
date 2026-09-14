#pragma once
#include <windows.h>

#include <string>

namespace yzhook
{
/*
   考试模式判定的强弱信号分级（依据 Yistart.exe 的 Organs\ 加载机制实测结论）。

   弱信号：启动阶段随 Organs\ 目录一并加载、常驻内存的模块（Exam.ads、ClassQuiz.ads）。
           它们只说明考试模块已就绪，不能作为"正在考试"的充要条件，
           因此只记日志、不触发自锁。

   强信号：真正进入考试/测验流程才会出现的证据，任一成立即熔断：
           1. 进程列表中出现独立考试进程 ExamDlg.exe；
           2. 进程内加载了考试对话框组件 ExamDlg.exe / ExamDlg.dll /
              ORAL_EXAM.ocx / ExamEditor.exe；
           3. 存在可见窗口，标题命中 考试|测验|答题|试卷|快问快答|Exam|Quiz，
              且该窗口属于远志安装目录。
*/
enum ExamReasonKind
{
    kExamReasonNone       = 0,   /* 未命中任何信号 */
    kExamReasonProcess    = 1,   /* 强信号：独立考试进程 ExamDlg.exe */
    kExamReasonModule     = 2,   /* 强信号：考试对话框组件已加载 */
    kExamReasonWindow     = 3,   /* 强信号：可见考试窗口标题命中 */
    kExamReasonWeakModule = 4    /* 弱信号：仅 exam.ads/ClassQuiz.ads 常驻 */
};

/* 考试检测的详细结果，供日志与诊断使用 */
struct ExamDetail
{
    int            reasonKind;   /* ExamReasonKind */
    const wchar_t* reason;       /* 命中的进程名/模块名/标题关键词 */
    std::wstring   moduleName;   /* 命中模块列表（含完整路径） */
    std::wstring   windowTitle;  /* reasonKind == kExamReasonWindow */
    std::wstring   windowPath;   /* 考试窗口/考试进程的完整路径 */

    ExamDetail() : reasonKind(kExamReasonNone), reason(nullptr) {}
};

/* 是否处于考试/测验模式；只认强信号，弱信号不会让它返回 true */
bool ExamDetect(const wchar_t** outReason);

/* 同上，但带回命中的进程名/模块名/窗口标题/窗口路径等细节 */
bool ExamDetect(ExamDetail* outDetail);

/* 弱信号检测：仅检查 Exam.ads / ClassQuiz.ads 是否常驻，命中不熔断，只用于日志 */
bool ExamDetectWeak(ExamDetail* outDetail, size_t* outCount);

/* 已加载的弱信号模块列表，形如 "exam.ads (C:/path/exam.ads)" */
std::wstring ExamLoadedModuleList();

/* 已加载的强信号模块列表，仅用于诊断 */
std::wstring ExamStrongModuleList();

/* 目标进程模块摘要，形如 "MainE.exe[pid] ..."，用于判断是否静态加载 */
std::wstring ModuleListDigest(size_t maxItems);

/* 组装可直接写日志/界面的多行诊断文本；cache 供 DLL 导出复用，可为 nullptr */
std::wstring ExamDetailText(const ExamDetail& detail, std::wstring* cache = nullptr);
} /* namespace yzhook */