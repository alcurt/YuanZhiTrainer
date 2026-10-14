#pragma once
//
// YZTrainer 主程序共享状态与模块接口。
//
#include <windows.h>
#include <string>
#include <vector>

#include "yz_protocol.h"
/* 产品版本号唯一来源是 common\yz_version.h；yz_protocol.h 把它派生为 YZ_VERSION_STR */

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
    /* 自动导出诊断包：广播常常没有预告、只有一次（2026-10-08 实测就漏了），所以
       ①首次识别到广播窗口时静默导出一份 ②退出时再导出一份。默认都开。 */
    bool         autoExportOnBroadcast;
    bool         autoExportOnExit;
    /* 首次发现采集组件（ExdDtDup/ScreenRecoder 等）时也静默导出一份：
       "教师监视时谁在抓屏"只在教师动手的那几秒可采，错过就得再等一节课。 */
    bool         autoExportOnCapture;
    /* 消息层钩子开关（v0.6.13 由编译期宏改成配置位）：1=把远志的 WH_GETMESSAGE /
       WH_CALLWNDPROC 也换成空过程（默认），0=只处理键盘/鼠标钩子。
       现场若出现"广播画面不动/窗口消息异常"，改成 0 重启即可二分，不必重编。 */
    bool         blockMessageHooks;
    /* DXGI 桌面复制取证（默认关）：只计数、不改变行为。现场用来判断"教师监视/广播
       到底有没有走桌面复制这条链"，也用来确认冻结 DXGI 不会误伤"接收广播"。 */
    bool         traceDesktopDuplication;
    /* 抓屏子进程自动注入（v0.6.32，默认开）：教师"监视/远程桌面"时学生端会拉起
       rundll32.exe + ExdDtDup.dll 做 DXGI 桌面复制，冻结必须在那个进程里做。
       设 0 时只识别、不注入（用于只想取证、不想动手的场合）。 */
    bool         injectCaptureChild;
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
    /* 只在"目标完整性高于本程序"时提示一次：这种情况注入、消息钩子、跨进程窗口写
       都会被 UIPI 挡住，必须改用 YZSysRun 以 SYSTEM 启动（09-29 机房实测结论）。 */
    bool             integrityHintLogged;
    /* 远程线程注入连续失败的次数与"换过目标就重新计数"用的 pid。
       连续失败到阈值就运行期自动改用消息钩子（不改 ini）：09-29 实测里受保护的客户端
       永远注入不进去，而消息钩子在同级/更高完整性下是通的。 */
    DWORD            injectFailStreak;
    /* v0.6.37：同一 pid 连续注入却始终不回报状态时的指数退避（§39.7：抓屏子进程连不上管道，
       旧逻辑会对它每 2 秒无限重注入） */
DWORD            injectBackoffPid;
DWORD            injectBackoffCount;
    DWORD            injectFailPid;
    bool             autoHookFallback;
    bool             autoExportedBroadcast;   /* "首次识别到广播窗口"那份是否已导过 */
    bool             autoExportedCapture;     /* "首次发现采集组件"那份是否已导过 */
    /* 冻结画面（防监视）期间把自己的窗口从画面里摘出去（v0.6.20）：
       冻结帧是目标进程里 GetDC(NULL)+BitBlt 抓的，主界面若可见就会被冻进去，
       教师在"监视/远程桌面"里一眼就能看见学生在用某个工具。下面两个字段只用于诊断与还原。 */
    bool             freezeWindowPrepared;    /* 已经做过"隐藏 + 亲和性"处理 */
    bool             freezeWindowWasVisible;  /* 处理前主界面是否可见（关闭冻结时按原样恢复） */
    bool             freezeAffinityOk;        /* WDA_EXCLUDEFROMCAPTURE 是否设置成功 */
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
/* 抓屏子进程出现时的处理（v0.6.32，实现在 injector.cpp）：记日志 + 按 PID 注入 */
void  OnCaptureChildSeen(DWORD pid, const std::wstring& line);
/* 把"当前要不要冻结抓屏进程"写到 C:\ProgramData\YZTrainer\capture-state.txt（v0.6.36）：
   抓屏子进程常连不上管道（服务端同时只服务一个客户端），这条是配置之外的补充通道。 */
void  WriteCaptureStateFile();
void  WatchdogTick();
std::wstring ResolveHookDllPath();

/* ---- ipc.cpp ---- */
bool IpcStart();
void IpcStop();
bool IpcIsConnected(DWORD* pid);
/* v0.6.38：某个 pid 是否已有连上的客户端（多客户端后"是否已注入"不能只看主客户端） */
bool IpcIsClientPidConnected(DWORD pid);
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

/* ---- 冻结画面期间的"别把自己拍进去"（ui.cpp 实现，v0.6.20） ----
   arming=true ：若主界面可见就收进托盘，并给主窗口设 WDA_EXCLUDEFROMCAPTURE，
                 然后等一小会儿让桌面重绘——**必须在下发冻结位之前调用**（见 CmdSetFlag 的注释）；
   arming=false：亲和性复位；处理前可见的话把主界面显示回来。
   两个字段（freezeWindowPrepared / freezeWindowWasVisible）只用于幂等与还原。 */
void FreezePrepareMainWindow(bool arming);

/* ---- servicepanel.cpp ---- */
void ServicePanelShow(HWND owner);

/* ---- diag.cpp ---- */
void ExportDiagnostics(HWND owner);
/* 静默自动导出（不开资源管理器、不弹窗）：广播首次出现 / 退出时各调一次 */
void AutoExportDiagnostics(const wchar_t* reason);
/* "广播首次出现"那份：带一次性守卫（每进程只导一次），evidence 只用于日志与快照 */
void AutoExportOnBroadcastOnce(const wchar_t* evidence);
/* "首次发现采集组件"那份：同样带一次性守卫（用于判定冻结画面该走 DXGI 还是 GDI） */
void AutoExportOnCaptureOnce(const wchar_t* evidence);


