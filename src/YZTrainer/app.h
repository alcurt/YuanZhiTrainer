#pragma once
//
// YZTrainer 主程序共享状态与模块接口。
//
#include <windows.h>
#include <string>
#include <vector>

#include "yz_protocol.h"
/* 产品版本号统一定义在 yz_protocol.h 的 YZ_VERSION_STR */

#define WM_YZ_STATUS (WM_APP + 1)
#define WM_YZ_LOG    (WM_APP + 2)
#define WM_YZ_EXAM   (WM_APP + 3)

/* 热键 ID */
#define ID_HOTKEY_WINDOWIZE 0x7101
#define ID_HOTKEY_UNLOCK    0x7102
#define ID_HOTKEY_ANTIMON   0x7103
#define ID_HOTKEY_SHOWUI    0x7104
#define ID_HOTKEY_FAKEFULL  0x7105

struct AppConfig
{
    DWORD        flags;            /* YZ_FLAG_* */
    DWORD        windowPercent;    /* 20..100 */
    int          logLevel;         /* 0..3 */
    bool         autoInject;       /* 是否自动注入/补注入 */
    int          injectMethod;     /* 0=远程线程（默认） 1=消息钩子（SetWindowsHookEx） */
    bool         enableExamGuard;  /* 是否启用考试模式强信号熔断（EnableExamGuard） */
    bool         externalWindowFix;/* 是否启用免注入的外部窗口纠正（ExternalWindowFix） */
    std::wstring targetDir;        /* 远志安装目录，空=按进程路径自动判定 */
    std::wstring hookDllPath;      /* 空=用内嵌资源；填路径则强制使用该文件 */
    std::vector<std::wstring> processNames;
};

struct AppRuntime
{
    AppConfig        cfg;
    HWND             hwndMain;
    HINSTANCE        hinst;
    std::wstring     exeDir;
    std::wstring     iniPath;
    std::wstring     hookDllSource;  /* embedded / ini / filedir，诊断用 */
    std::wstring     hookDllError;   /* 内嵌释放失败的原因原文 */

    HANDLE           pipe;
    CRITICAL_SECTION cs;
    DWORD            clientPid;
    bool             clientConnected;

    YZ_STATUS        status;
    bool             examMode;
    DWORD            targetPid;
    DWORD            injectCount;
    DWORD            lastInjectTick;
    HHOOK            injectHook;      /* 消息钩子注入方式下保存的 HHOOK */
};

extern AppRuntime g_app;

/* ---- config.cpp ---- */
std::wstring ConfigPath();
void ConfigLoad(AppConfig& cfg, const std::wstring& iniPath);
void ConfigSave(const AppConfig& cfg, const std::wstring& iniPath);
void ConfigApplyDefaults(AppConfig& cfg);

/* ---- injector.cpp ---- */
DWORD FindTargetProcess(const AppConfig& cfg);
bool  InjectHookDll(DWORD pid, const std::wstring& dllPath, std::wstring* err);
void  WatchdogTick();
std::wstring ResolveHookDllPath();

/* ---- ipc.cpp ---- */
bool IpcStart();
void IpcStop();
bool IpcIsConnected(DWORD* pid);
bool IpcSend(DWORD opcode, const void* payload, DWORD len);
bool IpcSendConfig();
bool IpcSendFlagCommand(DWORD opcode, bool on);
bool IpcSendUnload();

/* ---- ui.cpp ---- */
HWND UiInit(HINSTANCE hinst);
void UiShutdown();
void UiUpdateStatus();
void UiAppendLog(int level, const std::wstring& text);
void UiSyncControls();
void UiNotifyExam(bool exam);

/* ---- 命令处理（ui.cpp 调用） ---- */
void CmdSetFlag(DWORD flag, bool on);
void CmdToggleFlag(DWORD flag);
bool CmdGetFlag(DWORD flag);
void CmdInjectNow();
void CmdShowServicePanel();
void CmdExportDiag();
void CmdOpenLogFolder();
void CmdShowAbout();

/* ---- servicepanel.cpp ---- */
void ServicePanelShow(HWND owner);

/* ---- diag.cpp ---- */
void ExportDiagnostics(HWND owner);


