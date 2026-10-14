#include "app.h"

#include "yz_log.h"
#include "yz_util.h"

namespace
{
const wchar_t* kSection = L"General";

DWORD ReadInt(const std::wstring& ini, const wchar_t* key, DWORD def)
{
    return static_cast<DWORD>(GetPrivateProfileIntW(kSection, key, static_cast<INT>(def), ini.c_str()));
}

void WriteInt(const std::wstring& ini, const wchar_t* key, DWORD value)
{
    WritePrivateProfileStringW(kSection, key, yz::Format(L"%u", value).c_str(), ini.c_str());
}

/* 考试守护位必须与 cfg.enableExamGuard 始终一致——**两条路径都要走这里**。
   2026-10-08 修掉的 bug：ConfigApplyDefaults 只设了 enableExamGuard=true，而守卫位是在
   下面 ConfigLoad 里补的；当 ini 文件不存在（首次运行、只带了 exe 那种情形）时
   ConfigLoad 会在补位之前 early-return，于是钩子收到 flags=5、考试守护实际是关的
   —— 违反"考试/测验强信号必须熔断"这条硬边界。 */
void SyncExamGuardBit(AppConfig& cfg)
{
    if (cfg.enableExamGuard)
        cfg.flags |= YZ_CFG_EXAM_GUARD;
    else
        cfg.flags &= ~YZ_CFG_EXAM_GUARD;
}

/* 消息层钩子开关同样是控制位：钩子侧 v0.6.13 起按这个位决定要不要把远志的
   WH_GETMESSAGE/WH_CALLWNDPROC 也换成空过程。放 ini 里是为了现场能二分，
   不必改代码重编（原来那是 hooks_input.cpp 顶部的编译期宏）。 */
void SyncBlockMessageHooksBit(AppConfig& cfg)
{
    if (cfg.blockMessageHooks)
        cfg.flags |= YZ_CFG_BLOCK_MESSAGE_HOOKS;
    else
        cfg.flags &= ~YZ_CFG_BLOCK_MESSAGE_HOOKS;
}

/* 与 SyncBlockMessageHooksBit 同理：控制位必须单独同步进 flags，而且**必须在读到 ini 之后**再调
   （v0.6.13 的坑：先同步后读值 → ini 改成 0 不生效，回归阶段九抓出来的）。 */
void SyncTraceDuplicationBit(AppConfig& cfg)
{
    if (cfg.traceDesktopDuplication)
        cfg.flags |= YZ_CFG_TRACE_DUPLICATION;
    else
        cfg.flags &= ~YZ_CFG_TRACE_DUPLICATION;
}
} /* namespace */

std::wstring ConfigPath()
{
    return yz::JoinPath(yz::GetExeDir(), L"YZTrainer.ini");
}

void ConfigApplyDefaults(AppConfig& cfg)
{
    cfg.flags         = YZ_FLAG_WINDOWIZE | YZ_FLAG_INPUT_UNLOCK;
    cfg.windowPercent = 60;
    cfg.logLevel      = 2;
    cfg.autoInject    = true;
    cfg.injectMethod  = 0;
    cfg.enableExamGuard = true;
    cfg.externalWindowFix = true;
    cfg.autoExportOnBroadcast = true;
    cfg.autoExportOnExit = true;
    cfg.autoExportOnCapture = true;
    cfg.blockMessageHooks = true;
    cfg.traceDesktopDuplication = false;      /* 取证默认关：开了也只计数，不影响任何行为 */
    cfg.captureScanIntervalMs = 30000;   /* v0.6.40：采集栈监控扫描周期（默认 30 秒；回归里调小以省时间） */
    cfg.injectCaptureChild = true;   /* 抓屏子进程自动注入（v0.6.32，默认开）：教师监视/缩略图时
    SyncExamGuardBit(cfg);
    SyncBlockMessageHooksBit(cfg);
    SyncTraceDuplicationBit(cfg);
    cfg.processNames.clear();
    /* 客户端进程名单。网管版多一个守卫进程 Nmdeputy.exe，但它由注入器显式排除
       （IsGuardProcess），这里不放进来；需要时可在 INI 里自行添加。 */
    cfg.processNames.push_back(L"Yistart.exe");
    cfg.processNames.push_back(L"TEACHCMD.exe");
    cfg.processNames.push_back(L"PlayerGUI.exe");
    cfg.processNames.push_back(L"ExdPaintHelper.exe");
    cfg.processNames.push_back(L"YZSimTarget.exe");
}

void ConfigLoad(AppConfig& cfg, const std::wstring& iniPath)
{
    ConfigApplyDefaults(cfg);
    if (!yz::FileExists(iniPath))
        return;

    cfg.flags = ReadInt(iniPath, L"Flags", cfg.flags);
    cfg.windowPercent = ReadInt(iniPath, L"WindowPercent", cfg.windowPercent);
    if (cfg.windowPercent < 20) cfg.windowPercent = 20;
    if (cfg.windowPercent > 100) cfg.windowPercent = 100;
    cfg.logLevel = static_cast<int>(ReadInt(iniPath, L"LogLevel", static_cast<DWORD>(cfg.logLevel)));
    cfg.autoInject = ReadInt(iniPath, L"AutoInject", cfg.autoInject ? 1 : 0) != 0;
    cfg.injectMethod = static_cast<int>(ReadInt(iniPath, L"InjectMethod", static_cast<DWORD>(cfg.injectMethod)));
    cfg.enableExamGuard = ReadInt(iniPath, L"EnableExamGuard", cfg.enableExamGuard ? 1 : 0) != 0;
    cfg.externalWindowFix = ReadInt(iniPath, L"ExternalWindowFix", cfg.externalWindowFix ? 1 : 0) != 0;

    /* 考试守护开关以控制位形式随配置下发；其余功能位保持 ini 中的值。
       注意顺序：控制位必须**在读到对应的 ini 值之后**才同步——v0.6.13 第一版把
       SyncBlockMessageHooksBit 放在读 BlockMessageHooks 之前，结果 ini 改成 0 也没用
       （同步时用的还是默认 true），回归阶段九第二轮抓出来的。
       新增控制位时请一并检查这里：读值 → 同步位，两步顺序不能反。 */
    SyncExamGuardBit(cfg);
    cfg.autoExportOnBroadcast = ReadInt(iniPath, L"AutoExportOnBroadcast", cfg.autoExportOnBroadcast ? 1 : 0) != 0;
    cfg.autoExportOnExit = ReadInt(iniPath, L"AutoExportOnExit", cfg.autoExportOnExit ? 1 : 0) != 0;
    cfg.autoExportOnCapture = ReadInt(iniPath, L"AutoExportOnCapture", cfg.autoExportOnCapture ? 1 : 0) != 0;
    cfg.blockMessageHooks = ReadInt(iniPath, L"BlockMessageHooks", cfg.blockMessageHooks ? 1 : 0) != 0;
    cfg.traceDesktopDuplication = ReadInt(iniPath, L"TraceDesktopDuplication", cfg.traceDesktopDuplication ? 1 : 0) != 0;
    cfg.injectCaptureChild = ReadInt(iniPath, L"InjectCaptureChild", cfg.injectCaptureChild ? 1 : 0) != 0;
    cfg.captureScanIntervalMs = ReadInt(iniPath, L"CaptureScanIntervalMs", cfg.captureScanIntervalMs);
    SyncBlockMessageHooksBit(cfg);
    SyncTraceDuplicationBit(cfg);

    wchar_t buf[1024] = {0};
    GetPrivateProfileStringW(kSection, L"TargetDir", L"", buf, 1024, iniPath.c_str());
    cfg.targetDir = yz::Trim(buf);

    wchar_t names[1024] = {0};
    GetPrivateProfileStringW(kSection, L"ProcessNames", L"", names, 1024, iniPath.c_str());
    std::wstring nameList = yz::Trim(names);
    if (!nameList.empty())
    {
        cfg.processNames.clear();
        std::vector<std::wstring> parts = yz::SplitString(nameList, L';');
        for (size_t i = 0; i < parts.size(); i++)
            cfg.processNames.push_back(parts[i]);
    }

    wchar_t hook[1024] = {0};
    GetPrivateProfileStringW(kSection, L"HookDllPath", L"", hook, 1024, iniPath.c_str());
    cfg.hookDllPath = yz::Trim(hook);
}

void ConfigSave(const AppConfig& cfg, const std::wstring& iniPath)
{
    /* Flags 只落功能位；考试守护用独立键保存，避免同一开关存在两处事实来源 */
    WriteInt(iniPath, L"Flags", cfg.flags & YZ_FLAG_FUNCTION_MASK);
    WriteInt(iniPath, L"WindowPercent", cfg.windowPercent);
    WriteInt(iniPath, L"LogLevel", static_cast<DWORD>(cfg.logLevel));
    WriteInt(iniPath, L"AutoInject", cfg.autoInject ? 1 : 0);
    WriteInt(iniPath, L"InjectMethod", static_cast<DWORD>(cfg.injectMethod));
    WriteInt(iniPath, L"EnableExamGuard", cfg.enableExamGuard ? 1 : 0);
    WriteInt(iniPath, L"ExternalWindowFix", cfg.externalWindowFix ? 1 : 0);
    WriteInt(iniPath, L"AutoExportOnBroadcast", cfg.autoExportOnBroadcast ? 1 : 0);
    WriteInt(iniPath, L"AutoExportOnExit", cfg.autoExportOnExit ? 1 : 0);
    WriteInt(iniPath, L"AutoExportOnCapture", cfg.autoExportOnCapture ? 1 : 0);
    WriteInt(iniPath, L"BlockMessageHooks", cfg.blockMessageHooks ? 1 : 0);
    WriteInt(iniPath, L"TraceDesktopDuplication", cfg.traceDesktopDuplication ? 1 : 0);
    WriteInt(iniPath, L"InjectCaptureChild", cfg.injectCaptureChild ? 1 : 0);
    WriteInt(iniPath, L"CaptureScanIntervalMs", cfg.captureScanIntervalMs);
    WritePrivateProfileStringW(kSection, L"TargetDir", cfg.targetDir.c_str(), iniPath.c_str());
    WritePrivateProfileStringW(kSection, L"HookDllPath", cfg.hookDllPath.c_str(), iniPath.c_str());

    std::wstring names;
    for (size_t i = 0; i < cfg.processNames.size(); i++)
    {
        if (i != 0)
            names += L";";
        names += cfg.processNames[i];
    }
    WritePrivateProfileStringW(kSection, L"ProcessNames", names.c_str(), iniPath.c_str());
}
