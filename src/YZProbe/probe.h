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
    std::vector<ProbeModule> modules;
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
    std::vector<ProbeNet>      net;
    std::vector<std::wstring>  notes;
};

bool ProbeCollect(ProbeData& data, bool includeAllModules);

std::wstring ProbeToJson(const ProbeData& data);
std::wstring ProbeToMarkdown(const ProbeData& data);

bool ProbeIsYuanzhiPath(const std::wstring& path, const std::wstring& exeName);
bool ProbeReadExports(const std::wstring& filePath, std::vector<std::wstring>& out);

