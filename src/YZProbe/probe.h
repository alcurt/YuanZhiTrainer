#pragma once
//
// YZProbe 数据采集：进程/模块/窗口/服务/网络/自身令牌。
//
#include <windows.h>

#include <string>
#include <vector>

struct ProbeModule
{
    std::wstring name;
    std::wstring path;
    void*        base;              /* 模块基址，注入能力探测要用 */
    std::vector<std::wstring> exports;
};

struct ProbeProcess
{
    DWORD                    pid;
    std::wstring             name;
    std::wstring             path;
    std::wstring             user;
    DWORD                    integrity;
    bool                     elevated;
    bool                     isYuanzhi;
    DWORD                    protectionLevel;   /* PROCESS_PROTECTION_LEVEL_INFORMATION，0xFFFFFFFE=无保护 */
    /* 保护级别查询是否成功。false 时 protectionLevel 不可信（初值 0 恰好等于枚举里的
       PROTECTION_LEVEL_WINTCB_LIGHT，直接当级别显示会把"查不到"报成"PPL/WinTcbLight"——
       v0.6.23 之前就是这么错的：只有 isYuanzhi 的进程才查级别，而"因采集组件晋级"的进程
       那时还没被标成远志，于是现场最想看的那个抓屏进程被报成受 PPL 保护）。 */
    bool                     protectionKnown;
    /* 模块枚举是否成功。false 表示模块读不到（受保护/已退出），
       此时报告里的"内存未见 ExdDtDup"是"读不到"，不能当"没加载"用。 */
    bool                     modulesReadable;
    /* 全进程扫描的产物：该进程加载了采集组件（ExdDtDup/ExdElite/ExdPaint） */
    bool                     hasCaptureModule;
    std::wstring             captureModuleName;
    std::wstring             captureModulePath;
    std::vector<ProbeModule> modules;
    /* 进程模块读不到（受保护）时的兜底：直接读安装目录里的关键模块导出表 */
    std::vector<ProbeModule> diskModules;
};

struct ProbeWindow
{
    HWND         hwnd;
    DWORD        pid;
    std::wstring processName;
    std::wstring className;
    std::wstring title;
    RECT         rect;
    LONG         style;
    LONG         exStyle;
    bool         topmost;
    bool         visible;
    bool         borderless;
    bool         coversMonitor;
    bool         isYuanzhi;
    int          monitorIndex;
    RECT         monitorRect;
};

struct ProbeService
{
    std::wstring name;
    std::wstring display;
    std::wstring imagePath;
    DWORD        state;
    DWORD        startType;
    bool         isDriver;
    bool         isYuanzhi;
    DWORD        launchProtected;   /* SERVICE_CONFIG_LAUNCH_PROTECTED，0=未受保护启动 */
};

/* 逐权限位实测：单独请求某一位打开目标进程，再看句柄实得权限里有没有这一位。
   ObRegisterCallbacks 一类的驱动是"保住句柄、削掉权限位"，只看组合掩码
   分不清是哪几位被拿掉，必须逐位来测；只有这一列能证明"剥夺"而不是"没权限"。 */
struct ProbeAccessRight
{
    std::wstring name;          /* 权限名 */
    DWORD        requested;     /* 请求的位 */
    bool         opened;        /* OpenProcess 是否成功 */
    DWORD        err;           /* 失败时的 GetLastError */
    DWORD        granted;       /* 成功时 NtQueryObject 报告的实得权限 */
    DWORD        queryStatus;   /* NtQueryObject 返回的 NTSTATUS，非 0 便于排障 */
    bool         stripped;      /* 打开成功，但请求的位不在实得权限里 */
};

/* 对某个远志进程实测"能不能注入"：保护级别 + 句柄实际权限 + 逐项能力 + 逐权限位 */
struct ProbeAccess
{
    DWORD        pid;
    DWORD        protectionLevel;
    DWORD        grantedAccess;     /* NtQueryObject 报告的实际授予权限，0=查不到 */
    DWORD        grantedStatus;     /* 上面那次 NtQueryObject 的 NTSTATUS，0=成功 */
    std::wstring protectionText;
    std::wstring privNote;          /* SeDebug 启用情况等提示 */
    std::wstring moduleRead;        /* 模块枚举（Toolhelp 读目标内存） */
    std::wstring vmRead;            /* ReadProcessMemory */
    std::wstring vmOperation;       /* VirtualAllocEx（一页，随即释放） */
    std::wstring vmWrite;           /* WriteProcessMemory（只写自己刚分配的那页） */
    std::wstring createThread;      /* 默认不测，避免在目标进程留线程 */
    std::vector<ProbeAccessRight> rights;   /* 逐权限位实测 */
    std::wstring threadProbe;       /* 目标进程首个线程的句柄权限实测 */
};

/* 非微软签名的内核驱动（保护件/杀软/还原卡都在这张表里） */
struct ProbeDriver
{
    std::wstring name;
    std::wstring imagePath;
    std::wstring company;
    std::wstring fileVersion;   /* 文件版本，用于判断保护件的年代/来源 */
    DWORD        state;
};

struct ProbeNet
{
    DWORD        pid;
    std::wstring protocol;
    std::wstring local;
    std::wstring remote;
};

struct ProbeSelf
{
    DWORD                    pid;
    std::wstring             path;
    std::wstring             arch;
    std::wstring             user;
    DWORD                    integrity;
    bool                     elevated;
    bool                     hasSeDebug;
    std::vector<ProbeModule> modules;
};

struct ProbeData
{
    std::wstring               generatedAt;
    std::wstring               computerName;
    std::wstring               osVersion;
    ProbeSelf                  self;
    std::vector<ProbeProcess>  processes;
    std::vector<ProbeWindow>   windows;
    std::vector<ProbeService>  services;
    std::vector<ProbeDriver>   drivers;
    std::vector<ProbeAccess>   access;
    std::vector<ProbeNet>      net;
    std::vector<std::wstring>  notes;
};

bool ProbeCollect(ProbeData& data, bool includeAllModules);

/* 只取模块名（不保留列表）——全进程扫"采集组件"用，报告体积不受影响 */
bool ProbeReadModuleNames(DWORD pid, std::vector<std::wstring>& out);

/* 扫一个进程有没有加载采集组件（ExdDtDup/ExdElite/ExdPaint）；命中回报名字与路径 */
bool ProbeScanCaptureModule(DWORD pid, std::wstring* outName, std::wstring* outPath);

/* 采集栈一行判词（与主程序诊断包用同一套判词，见 common/yz_capture.h） */
std::wstring ProbeCaptureStackLine(DWORD pid, const std::wstring& exeName,
                                   const std::wstring& exePath,
                                   const std::vector<std::wstring>& moduleNames);

/* 进程清单（--watch 常驻模式用，只要 pid/名字/路径） */
struct ProbeProcBrief
{
    DWORD        pid;
    std::wstring name;
    std::wstring path;
};
bool ProbeSnapshotProcesses(std::vector<ProbeProcBrief>& out);

std::wstring ProbeToJson(const ProbeData& data);
std::wstring ProbeToMarkdown(const ProbeData& data);

bool ProbeIsYuanzhiPath(const std::wstring& path, const std::wstring& exeName);
bool ProbeReadExports(const std::wstring& filePath, std::vector<std::wstring>& out);

/* 对指定 PID 单独做一次注入能力探测（现场排障用，--access <pid>） */
void ProbeAccessProbePid(DWORD pid, ProbeData& data);

