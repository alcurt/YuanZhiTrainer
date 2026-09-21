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

    /* 考试守护开关以控制位形式随配置下发；其余功能位保持 ini 中的值 */
    if (cfg.enableExamGuard)
        cfg.flags |= YZ_CFG_EXAM_GUARD;
    else
        cfg.flags &= ~YZ_CFG_EXAM_GUARD;

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
