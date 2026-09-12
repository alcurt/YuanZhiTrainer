#pragma once
//
// 统一定位到 %TEMP%\YZTrainer\yzt.log 的轻量日志。
// 单个文件超过 2MB 时自动换用 yzt-1.log / yzt-2.log ...（永不删除旧文件）。
//
#include <windows.h>
#include <string>

namespace yz
{
enum
{
    kLogError = 0,
    kLogWarn  = 1,
    kLogInfo  = 2,
    kLogDebug = 3
};

typedef void (*LogSink)(int level, const wchar_t* text, void* ctx);

void LogInit(const wchar_t* tag);
void LogShutdown();

void LogSetLevel(int level);
int  LogLevel();

void LogSetSink(LogSink sink, void* ctx);

void LogWrite(int level, const wchar_t* fmt, ...);

std::wstring LogDir();
std::wstring LogFilePath();
} /* namespace yz */

#define YZLOGE(...) yz::LogWrite(yz::kLogError, __VA_ARGS__)
#define YZLOGW(...) yz::LogWrite(yz::kLogWarn,  __VA_ARGS__)
#define YZLOGI(...) yz::LogWrite(yz::kLogInfo,  __VA_ARGS__)
#define YZLOGD(...) yz::LogWrite(yz::kLogDebug, __VA_ARGS__)
