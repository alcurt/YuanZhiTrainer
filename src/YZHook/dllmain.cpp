//
// YZHook.dll —— 注入到远志学生端进程内的用户态 Hook 模块。
//
// 设计要点：
//   * DllMain 只做最小工作，真正的初始化在独立线程里完成，避免加载器锁问题。
//   * 宿主进程不是远志学生端（或 YZSimTarget 模拟目标）时立即放弃，不做任何 hook。
//   * 不写磁盘、不装驱动；退出时恢复策略值并卸载全部 hook。
//
#include <windows.h>

#include "engine.h"
#include "exam.h"
#include "yz_hook_state.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);

    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        yz::LogInit(L"YZHook");
        yzhook::EngineStart();
        break;

    case DLL_PROCESS_DETACH:
        /* 进程退出阶段不再做复杂清理，交给引擎线程自身的退出路径 */
        yz::LogShutdown();
        break;

    default:
        break;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) DWORD WINAPI YZ_ProtocolVersion()
{
    return YZ_PROTOCOL_VERSION;
}

extern "C" __declspec(dllexport) BOOL WINAPI YZ_Init(const YZ_CONFIG* cfg)
{
    if (cfg == nullptr)
        return FALSE;
    yzhook::EngineApplyConfig(*cfg);
    return TRUE;
}

extern "C" __declspec(dllexport) void WINAPI YZ_Shutdown()
{
    yzhook::EngineStop();
}

extern "C" __declspec(dllexport) BOOL WINAPI YZ_ApplyCmd(DWORD opcode, DWORD value)
{
    switch (opcode)
    {
    case YZ_CMD_SET_WINDOW_MODE:  yzhook::EngineSetFlag(YZ_FLAG_WINDOWIZE,    value != 0); return TRUE;
    case YZ_CMD_SET_INPUT_UNLOCK: yzhook::EngineSetFlag(YZ_FLAG_INPUT_UNLOCK, value != 0); return TRUE;
    case YZ_CMD_SET_ANTI_MONITOR: yzhook::EngineSetFlag(YZ_FLAG_ANTI_MONITOR, value != 0); return TRUE;
    case YZ_CMD_SET_BLOCK_REMOTE: yzhook::EngineSetFlag(YZ_FLAG_BLOCK_REMOTE, value != 0); return TRUE;
    default:
        return FALSE;
    }
}

extern "C" __declspec(dllexport) BOOL WINAPI YZ_GetStatus(YZ_STATUS* status)
{
    if (status == nullptr)
        return FALSE;
    yzhook::EngineFillStatus(status);
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL WINAPI YZ_IsExamMode()
{
    return yzhook::EngineIsExamMode() ? TRUE : FALSE;
}

/* 诊断辅助：返回本次检测的详细信息文本（含命中原因、窗口标题、模块列表） */
extern "C" __declspec(dllexport) const wchar_t* WINAPI YZ_ExamDetailText()
{
    static std::wstring s_cache;
    yzhook::ExamDetail detail;
    yzhook::ExamDetect(&detail);
    yzhook::ExamDetailText(detail, &s_cache);
    return s_cache.c_str();
}

/* 诊断辅助：弱信号（Exam.ads / ClassQuiz.ads 常驻）单独查询，便于自动测试 */
extern "C" __declspec(dllexport) const wchar_t* WINAPI YZ_ExamWeakDetailText()
{
    static std::wstring s_weak;
    yzhook::ExamDetail detail;
    size_t count = 0;
    yzhook::ExamDetectWeak(&detail, &count);
    s_weak = yz::Format(L"弱信号命中=%s 命中模块=%s",
                        (count != 0) ? L"true" : L"false",
                        detail.moduleName.empty() ? L"(无)" : detail.moduleName.c_str());
    return s_weak.c_str();
}

/* 诊断辅助：只返回是否处于考试模式，供外部工具核对 */
extern "C" __declspec(dllexport) BOOL WINAPI YZ_ExamDetailHit()
{
    return yzhook::ExamDetect(static_cast<const wchar_t**>(nullptr)) ? TRUE : FALSE;
}

/* 备用注入路径：YZTrainer 用 SetWindowsHookEx(WH_GETMESSAGE) 把本 DLL 挂进目标进程时，
   Windows 会调用这个导出。真正的初始化在 DllMain 里已完成（EngineStart），
   这里只把消息传下去，保持钩子有效；宿主识别失败时它在别的进程里也只是空转。 */
extern "C" __declspec(dllexport) LRESULT CALLBACK YZ_HookProc(int code, WPARAM wParam, LPARAM lParam)
{
    return CallNextHookEx(nullptr, code, wParam, lParam);
}
