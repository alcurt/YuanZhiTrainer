#pragma once
//
// YZTrainer 进程间协议：主程序 <-> 注入到远志学生端的 Hook DLL
//
// 帧格式：[u32 magic 'YZT1'][u32 opcode][u32 payloadLen][payload]
//
#include <windows.h>
#include <vector>

#include "yz_version.h"

#define YZ_PROTOCOL_VERSION 1u
/* 产品版本（语义化版本）唯一来源是 yz_version.h：数值宏给 .rc 用，这里派生宽字符串给
   日志/报告用。下面这条 static_assert 是"防脱节闸门"——只改数值或只改字符串都会编译失败
   （历史上出过连续 6 个版本只改字符串、数值字段停在旧值的事故）。
   注意与上面的 YZ_PROTOCOL_VERSION（管道/结构体协议版本）区分开。 */
#define YZ_STRINGIZE2(x) #x
#define YZ_STRINGIZE(x)  YZ_STRINGIZE2(x)
#define YZ_VERSION_STR_DERIVED_NARROW \
    YZ_STRINGIZE(YZ_VERSION_MAJOR) "." YZ_STRINGIZE(YZ_VERSION_MINOR) "." YZ_STRINGIZE(YZ_VERSION_PATCH)

namespace yz_version_guard
{
inline constexpr bool SameString(const char* a, const char* b)
{
    while (*a != '\0' && *a == *b)
    {
        ++a;
        ++b;
    }
    return *a == *b;
}
static_assert(SameString(YZ_VERSION_STR_DERIVED_NARROW, YZ_VERSION_STR_NARROW),
              "yz_version.h: 字符串版本与数值宏不一致（改版本时两处必须一起改）");
} /* namespace yz_version_guard */

/* 注意顺序：数字没有"宽字面量"，L##0 只会拼成标识符 L0。必须先把数字 stringize 成
   窄字符串，再把 L 拼到字符串字面量前面。 */
#define YZ_WIDEN2(s) L##s
#define YZ_WIDEN(s)  YZ_WIDEN2(s)
#define YZ_VERSION_STR                                                       \
    YZ_WIDEN(YZ_STRINGIZE(YZ_VERSION_MAJOR)) L"." YZ_WIDEN(YZ_STRINGIZE(YZ_VERSION_MINOR)) \
    L"." YZ_WIDEN(YZ_STRINGIZE(YZ_VERSION_PATCH))
#define YZ_PIPE_NAME        L"\\\\.\\pipe\\YZTrainer"
#define YZ_FRAME_MAGIC      0x31545A59u  /* 'YZT1' */
#define YZ_MAX_LOG_TEXT     400
#define YZ_MAX_FRAME_PAYLOAD 65536u

/* 功能开关（YZ_CONFIG::flags / YZ_STATUS::flags） */
#define YZ_FLAG_WINDOWIZE    0x00000001u  /* 全屏广播窗口化 */
#define YZ_FLAG_TOPMOST      0x00000002u  /* 窗口化后保持置顶 */
#define YZ_FLAG_INPUT_UNLOCK 0x00000004u  /* 解除键鼠锁定 */
#define YZ_FLAG_ANTI_MONITOR 0x00000008u  /* 冻结教师端看到的画面 */
#define YZ_FLAG_BLOCK_REMOTE 0x00000010u  /* 拦截教师端遥控输入 */
#define YZ_FLAG_FAKE_FULLSCREEN 0x00000020u /* 假全屏：保持全屏外观，只取消置顶（优先于 WINDOWIZE） */

/* 控制位：不属于"功能开关"，不参与 g_flags 上报，只影响引擎行为 */
#define YZ_CFG_EXAM_GUARD    0x00010000u  /* 1=启用考试模式强信号熔断，0=完全跳过检测 */
#define YZ_CFG_BLOCK_MESSAGE_HOOKS 0x00020000u /* 1=把远志的 WH_GETMESSAGE/WH_CALLWNDPROC 也换成空过程（默认），0=只处理键盘/鼠标钩子（现场二分用） */
#define YZ_FLAG_FUNCTION_MASK 0x0000FFFFu /* 功能开关掩码，用于剔除控制位 */

enum YZ_OPCODE : DWORD
{
    /* 主程序 -> Hook DLL */
    YZ_CMD_SET_WINDOW_MODE  = 1,
    YZ_CMD_SET_INPUT_UNLOCK = 2,
    YZ_CMD_SET_ANTI_MONITOR = 3,
    YZ_CMD_SET_BLOCK_REMOTE = 4,
    YZ_CMD_QUERY_STATUS     = 5,
    YZ_CMD_UNLOAD           = 6,
    YZ_CMD_APPLY_CONFIG     = 7,
    YZ_CMD_SET_FAKE_FULL    = 8,

    /* Hook DLL -> 主程序 */
    YZ_EVT_HELLO     = 100,
    YZ_EVT_STATUS    = 101,
    YZ_EVT_LOG       = 102,
    YZ_EVT_EXAM_MODE = 103
};

enum YZ_LOG_LEVEL : DWORD
{
    YZ_LOG_ERROR = 0,
    YZ_LOG_WARN  = 1,
    YZ_LOG_INFO  = 2,
    YZ_LOG_DEBUG = 3
};

#pragma pack(push, 1)

struct YZ_FRAME_HEADER
{
    DWORD magic;
    DWORD opcode;
    DWORD length;
};

struct YZ_CONFIG
{
    DWORD size;
    DWORD version;
    DWORD flags;
    DWORD windowPercent;   /* 窗口化后的宽度占显示器百分比，默认 60 */
};

struct YZ_STATUS
{
    DWORD   size;
    DWORD   version;
    DWORD   pid;
    DWORD   flags;
    DWORD   hooksInstalled;
    DWORD   windowizeCount;
    DWORD   examMode;
    DWORD   lastError;
    wchar_t hostPath[MAX_PATH];
};

struct YZ_LOG_EVENT
{
    DWORD   size;
    DWORD   level;
    DWORD   pid;
    wchar_t text[YZ_MAX_LOG_TEXT];
};

/* IPC 结构体是按字节流跨进程传的（主程序里内嵌的 Hook DLL 与主程序必须完全一致），
   所以用编译期断言把尺寸钉死：一旦有人加字段/改宽度，**编译就失败**，而不是等到现场
   出现字段错位、状态读成垃圾值。动结构体时必须同时递增 YZ_PROTOCOL_VERSION，
   并在接收侧校验 cfg.version / cfg.size（见 engine.cpp 的 YZ_CMD_APPLY_CONFIG）。 */
static_assert(sizeof(YZ_FRAME_HEADER) == 12, "YZ_FRAME_HEADER 必须是 12 字节");
static_assert(sizeof(YZ_CONFIG) == 16,
              "YZ_CONFIG 尺寸变了：请递增 YZ_PROTOCOL_VERSION 并同步两端");
static_assert(sizeof(YZ_STATUS) == 8 * sizeof(DWORD) + MAX_PATH * sizeof(wchar_t),
              "YZ_STATUS 尺寸变了：请递增 YZ_PROTOCOL_VERSION 并同步两端");
static_assert(sizeof(YZ_LOG_EVENT) == 3 * sizeof(DWORD) + YZ_MAX_LOG_TEXT * sizeof(wchar_t),
              "YZ_LOG_EVENT 尺寸变了：请递增 YZ_PROTOCOL_VERSION 并同步两端");

#pragma pack(pop)

namespace yz
{
inline bool ReadExact(HANDLE h, void* buf, DWORD len)
{
    BYTE* p = static_cast<BYTE*>(buf);
    DWORD done = 0;
    while (done < len)
    {
        DWORD got = 0;
        if (!ReadFile(h, p + done, len - done, &got, nullptr) || got == 0)
            return false;
        done += got;
    }
    return true;
}

inline bool WriteExact(HANDLE h, const void* buf, DWORD len)
{
    const BYTE* p = static_cast<const BYTE*>(buf);
    DWORD done = 0;
    while (done < len)
    {
        DWORD wrote = 0;
        if (!WriteFile(h, p + done, len - done, &wrote, nullptr) || wrote == 0)
            return false;
        done += wrote;
    }
    return true;
}

inline bool SendFrame(HANDLE h, DWORD opcode, const void* payload, DWORD len)
{
    YZ_FRAME_HEADER hdr;
    hdr.magic  = YZ_FRAME_MAGIC;
    hdr.opcode = opcode;
    hdr.length = len;
    if (!WriteExact(h, &hdr, sizeof(hdr)))
        return false;
    if (len != 0 && payload != nullptr)
        return WriteExact(h, payload, len);
    return true;
}

inline bool RecvFrame(HANDLE h, DWORD* opcode, std::vector<BYTE>* payload)
{
    YZ_FRAME_HEADER hdr;
    if (!ReadExact(h, &hdr, sizeof(hdr)))
        return false;
    if (hdr.magic != YZ_FRAME_MAGIC)
        return false;
    if (hdr.length > YZ_MAX_FRAME_PAYLOAD)
        return false;
    payload->resize(hdr.length);
    if (hdr.length != 0 && !ReadExact(h, payload->data(), hdr.length))
        return false;
    if (opcode != nullptr)
        *opcode = hdr.opcode;
    return true;
}
} /* namespace yz */
