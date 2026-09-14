#pragma once
//
// YZTrainer 进程间协议：主程序 <-> 注入到远志学生端的 Hook DLL
//
// 帧格式：[u32 magic 'YZT1'][u32 opcode][u32 payloadLen][payload]
//
#include <windows.h>
#include <vector>

#define YZ_PROTOCOL_VERSION 1u
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

/* 控制位：不属于"功能开关"，不参与 g_flags 上报，只影响引擎行为 */
#define YZ_CFG_EXAM_GUARD    0x00010000u  /* 1=启用考试模式强信号熔断，0=完全跳过检测 */
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
