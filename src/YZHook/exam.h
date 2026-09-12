#pragma once
#include <windows.h>

namespace yzhook
{
/* 是否处于考试/测验模式（远志考试模块已加载或出现考试窗口标题） */
bool ExamDetect(const wchar_t** outReason);
} /* namespace yzhook */
