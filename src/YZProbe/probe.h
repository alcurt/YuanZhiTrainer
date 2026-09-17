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

/* 对某个远志进程实测"能不能注入"：保护级别 + 句柄实际权限 + 逐项能力 */
struct ProbeAccess
{
    DWORD        pid;
    DWORD        protectionLevel;
    DWORD        grantedAccess;     /* NtQueryObject 报告的实际授予权限，0=查不到 */
    std::wstring protectionText;
    std::wstring privNote;          /* SeDebug 启用情况等提示 */
    std::wstring moduleRead;        /* 模块枚举（Toolhelp 读目标内存） */
    std::wstring vmRead;            /* ReadProcessMemory */
    std::wstring vmOperation;       /* VirtualAllocEx（一页，随即释放） */
    std::wstring vmWrite;           /* WriteProcessMemory（只写自己刚分配的那页） */
    std::wstring createThread;      /* 默认不测，避免在目标进程留线程 */
};

/* 非微软签名的内核驱动（保护件/杀软/还原卡都在这张表里） */
struct ProbeDriver
{
    std::wstring name;
    std::wstring imagePath;
    std::wstring company;
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

std::wstring ProbeToJson(const ProbeData& data);
std::wstring ProbeToMarkdown(const ProbeData& data);

bool ProbeIsYuanzhiPath(const std::wstring& path, const std::wstring& exeName);
bool ProbeReadExports(const std::wstring& filePath, std::vector<std::wstring>& out);

/* 对指定 PID 单独做一次注入能力探测（现场排障用，--access <pid>） */
void ProbeAccessProbePid(DWORD pid, ProbeData& data);

