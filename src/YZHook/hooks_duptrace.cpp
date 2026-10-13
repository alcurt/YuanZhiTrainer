//
// DXGI 桌面复制"取证"钩子（v0.6.21）——**只计数，不改变任何行为**。
//
// 为什么需要它：教师"监视/远程桌面"到底走哪条采集路径（DXGI 还是 GDI helper）决定冻结画面
// 该怎么实现；而更危险的一个问题是——学生机上的 `DuplicateOutput` 会不会**也被"接收广播"
// 那条链用到**。如果会，那在错误的链上冻结就会把学生看教师广播的画面冻住（本机复现不了、
// 现场难排查）。这条钩子就是去回答这两个问题的：它记录"谁建立了桌面复制、取帧有多频繁"，
// 从不拦截、从不改返回值、从不 sleep。
//
// 安全设计：
//   * 默认关闭（控制位 YZ_CFG_TRACE_DUPLICATION，出厂 ini 里是 0），关闭时本模块**什么都不做**；
//   * `dxgi.dll` 用 LoadLibrary 动态取，头文件只借类型声明 → 不给 YZHook.dll 增加任何静态导入，
//     关闭时也不会把 dxgi 拖进目标进程；
//   * 只替换 `IDXGIOutput1` 真 vtable 里的槽 22（0x58 `DuplicateOutput`），
//     duplication 对象则按对象换 vtable（拷贝表、只换槽 8/14）——不动 dxgi.dll 里的共享表；
//   * 不自己调用 `DuplicateOutput`（那会占掉"每输出仅一个客户端"的名额，把厂商挤掉）。
//
#include "hooks_duptrace.h"

#include "yz_hook_state.h"

#include <dxgi1_2.h>

namespace yzhook
{
/* 冻结开关（v0.6.33）：变量放最前，AcquireNextFrame 桩与引擎侧共用 */
volatile LONG g_freezeDup = 0;
namespace
{
/* ---- 真函数指针（从真 vtable 里取） ---- */
typedef HRESULT (STDMETHODCALLTYPE *PFN_DuplicateOutput)(IDXGIOutput1*, IUnknown*, IDXGIOutputDuplication**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_AcquireNextFrame)(IDXGIOutputDuplication*, UINT, DXGI_OUTDUPL_FRAME_INFO*, IDXGIResource**);
typedef HRESULT (STDMETHODCALLTYPE *PFN_ReleaseFrame)(IDXGIOutputDuplication*);

PFN_DuplicateOutput g_realDuplicate = nullptr;
PFN_AcquireNextFrame g_realAcquire   = nullptr;
PFN_ReleaseFrame     g_realRelease   = nullptr;

/* 槽位（开发记录 §19 本机功能验证过） */
const int kSlotOutput1Duplicate = 22;   /* 0x58 */
const int kSlotDupAcquire       = 8;    /* 0x20 */
const int kSlotDupRelease       = 14;   /* 0x38 */
const int kSlots                = 24;   /* 够覆盖 IDXGIOutputDuplication 的全部方法 */

void* g_dupVtblCopy[kSlots];            /* 给 duplication 对象用的表（含我们的两个桩） */
bool  g_installed = false;

/* ---- 统计 ---- */
volatile LONG g_dupCreated      = 0;
volatile LONG g_dupFailed       = 0;
volatile LONG g_acquireCalls    = 0;
volatile LONG g_acquireTimeouts = 0;
volatile LONG g_acquireFrames   = 0;
volatile LONG g_releaseCalls    = 0;
volatile LONG g_summaries       = 0;
DWORD         g_lastSummaryTick = 0;
DWORD         g_firstCallTick   = 0;

PFN_AcquireNextFrame g_stubAcquire = nullptr;   /* 前向声明用 */
PFN_ReleaseFrame     g_stubRelease = nullptr;
HRESULT STDMETHODCALLTYPE TraceAcquireNextFrame(IDXGIOutputDuplication*, UINT, DXGI_OUTDUPL_FRAME_INFO*, IDXGIResource**);
HRESULT STDMETHODCALLTYPE TraceReleaseFrame(IDXGIOutputDuplication*);

/* 30 秒一条统计：让现场日志能看出"这条链到底有没有在取帧、多快" */
void MaybeSummarize(const wchar_t* why)
{
    const DWORD now = GetTickCount();
    if (g_firstCallTick == 0)
        g_firstCallTick = now;
    if (g_lastSummaryTick != 0 && (now - g_lastSummaryTick) < 30000)
        return;
    g_lastSummaryTick = now;
    const LONG n = InterlockedIncrement(&g_summaries);
    const DWORD span = (now > g_firstCallTick) ? (now - g_firstCallTick) : 1;
    const LONG frames = InterlockedCompareExchange(&g_acquireFrames, 0, 0);
    const LONG calls  = InterlockedCompareExchange(&g_acquireCalls, 0, 0);
    YZLOGI(L"[DXGI 取证] %s 取帧统计 #%d: 调用=%d 超时=%d 取到帧=%d（约 %d 帧/秒，累计 %u 秒）",
           why, n, calls, InterlockedCompareExchange(&g_acquireTimeouts, 0, 0), frames,
           static_cast<int>(frames * 1000 / static_cast<LONG>(span)), span / 1000);
}

/* 按对象换 vtable：拷贝真表、只替换槽 8/14。真表是进程内共享的，所以只取一次原指针。 */
void PatchDuplicationObject(IDXGIOutputDuplication* obj)
{
    if (obj == nullptr)
        return;
    void** realVtbl = *reinterpret_cast<void***>(obj);
    if (realVtbl == nullptr)
        return;

    if (g_realAcquire == nullptr)
    {
        g_realAcquire = reinterpret_cast<PFN_AcquireNextFrame>(realVtbl[kSlotDupAcquire]);
        g_realRelease = reinterpret_cast<PFN_ReleaseFrame>(realVtbl[kSlotDupRelease]);
        if (g_realAcquire == nullptr || g_realRelease == nullptr)
        {
            YZLOGW(L"[DXGI 取证] duplication vtable 槽位取指针失败，放弃取证（不影响厂商行为）");
            g_realAcquire = nullptr;
            return;
        }
        for (int i = 0; i < kSlots; ++i)
            g_dupVtblCopy[i] = realVtbl[i];
        g_dupVtblCopy[kSlotDupAcquire] = reinterpret_cast<void*>(&TraceAcquireNextFrame);
        g_dupVtblCopy[kSlotDupRelease] = reinterpret_cast<void*>(&TraceReleaseFrame);
    }
    *reinterpret_cast<void**>(obj) = reinterpret_cast<void*>(g_dupVtblCopy);
}

HRESULT STDMETHODCALLTYPE TraceDuplicateOutput(IDXGIOutput1* self, IUnknown* device, IDXGIOutputDuplication** out)
{
    if (g_realDuplicate == nullptr)
        return E_FAIL;

    if (!TryEnterHook())
        return g_realDuplicate(self, device, out);      /* 重入时只转发 */

    const HRESULT hr = g_realDuplicate(self, device, out);
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr)
    {
        const LONG n = InterlockedIncrement(&g_dupCreated);
        PatchDuplicationObject(*out);
        if (n <= 3)
            YZLOGI(L"[DXGI 取证] 建立桌面复制成功（第 %d 次，duplication=%p）——这条链上的取帧会被计数，但行为不受影响",
                   n, *out);
    }
    else if (FAILED(hr))
    {
        InterlockedIncrement(&g_dupFailed);
        YZLOGI(L"[DXGI 取证] DuplicateOutput 失败 hr=0x%08X（0x887A0022=已被别的客户端占用）",
               static_cast<unsigned>(hr));
    }

    LeaveHook();
    return hr;
}



HRESULT STDMETHODCALLTYPE TraceAcquireNextFrame(IDXGIOutputDuplication* self, UINT timeout,
                                                DXGI_OUTDUPL_FRAME_INFO* info, IDXGIResource** res)
{
    if (g_realAcquire == nullptr)
        return E_FAIL;

    if (!TryEnterHook())
        return g_realAcquire(self, timeout, info, res);

    if (InterlockedCompareExchange(&g_freezeDup, 0, 0) != 0)
    {
        /* 冻结中：**诚实等待**再回 WAIT_TIMEOUT（不伪造帧、不排空）。等待长度按调用方 timeout 取，
           0 时给 8 毫秒——不然调用方会变成忙等（§27 实测 3.54M 次/秒）。 */
        DWORD waitMs = (timeout == 0) ? 8 : timeout;
        if (waitMs > 100)
            waitMs = 100;
        Sleep(waitMs);
        InterlockedIncrement(&g_acquireCalls);
        InterlockedIncrement(&g_acquireTimeouts);
        LeaveHook();
        return DXGI_ERROR_WAIT_TIMEOUT;
    }

    InterlockedIncrement(&g_acquireCalls);
    const HRESULT hr = g_realAcquire(self, timeout, info, res);   /* 原样转发，返回值绝不改 */
    if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        InterlockedIncrement(&g_acquireTimeouts);
    else if (SUCCEEDED(hr))
        InterlockedIncrement(&g_acquireFrames);
    MaybeSummarize(L"周期");

    LeaveHook();
    return hr;
}

HRESULT STDMETHODCALLTYPE TraceReleaseFrame(IDXGIOutputDuplication* self)
{
    InterlockedIncrement(&g_releaseCalls);
    return (g_realRelease != nullptr) ? g_realRelease(self) : E_FAIL;
}
} /* namespace */

bool DupTraceInstall(bool enableNow)
{
    if (!enableNow)
        return true;
    if (g_installed)
        return true;

    /* 只借 dxgi.dll 拿一个 IDXGIOutput1 实例来读它的 vtable；**不调用 DuplicateOutput**。
       动态加载是刻意的：不给 YZHook.dll 增加静态导入，关闭取证时不会把 dxgi 拖进目标进程。 */
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    if (dxgi == nullptr)
        dxgi = LoadLibraryW(L"dxgi.dll");
    if (dxgi == nullptr)
    {
        YZLOGW(L"[DXGI 取证] 加载 dxgi.dll 失败，取证不启用");
        return false;
    }

    typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void**);
    PFN_CreateDXGIFactory1 createFactory =
        reinterpret_cast<PFN_CreateDXGIFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    if (createFactory == nullptr)
    {
        YZLOGW(L"[DXGI 取证] 取 CreateDXGIFactory1 失败，取证不启用");
        return false;
    }

    IDXGIFactory1* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    IDXGIOutput*   output  = nullptr;
    IDXGIOutput1*  output1 = nullptr;
    bool ok = false;
    void** vtbl = nullptr;

    if (SUCCEEDED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) &&
        factory != nullptr &&
        SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && adapter != nullptr &&
        SUCCEEDED(adapter->EnumOutputs(0, &output)) && output != nullptr &&
        SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1))) &&
        output1 != nullptr)
    {
        vtbl = *reinterpret_cast<void***>(output1);
        g_realDuplicate = reinterpret_cast<PFN_DuplicateOutput>(vtbl[kSlotOutput1Duplicate]);
        ok = (g_realDuplicate != nullptr && vtbl[kSlotOutput1Duplicate] != nullptr);
    }

    if (ok)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(&vtbl[kSlotOutput1Duplicate], sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            vtbl[kSlotOutput1Duplicate] = reinterpret_cast<void*>(&TraceDuplicateOutput);
            DWORD tmp = 0;
            VirtualProtect(&vtbl[kSlotOutput1Duplicate], sizeof(void*), oldProtect, &tmp);
            FlushInstructionCache(GetCurrentProcess(), &vtbl[kSlotOutput1Duplicate], sizeof(void*));
            g_installed = true;
            YZLOGI(L"[DXGI 取证] 已装上桌面复制取证钩子（槽 0x58，只计数不改行为）");
        }
        else
        {
            YZLOGW(L"[DXGI 取证] VirtualProtect 失败 err=%u，取证不启用", GetLastError());
        }
    }
    else
    {
        YZLOGW(L"[DXGI 取证] 未能取到 IDXGIOutput1 vtable，取证不启用（目标可能没有可用输出）");
    }

    if (output1 != nullptr) output1->Release();
    if (output  != nullptr) output->Release();
    if (adapter != nullptr) adapter->Release();
    if (factory != nullptr) factory->Release();
    return g_installed;
}

void DupTraceShutdown()
{
    /* 只计数不改变行为，所以退出时不必须摘钩；这里仅把统计打一条尾巴便于现场看总账。 */
    if (g_installed)
    {
        YZLOGI(L"[DXGI 取证] 收尾: 建立复制=%d 失败=%d 取帧调用=%d 超时=%d 取到帧=%d 释放=%d",
               InterlockedCompareExchange(&g_dupCreated, 0, 0),
               InterlockedCompareExchange(&g_dupFailed, 0, 0),
               InterlockedCompareExchange(&g_acquireCalls, 0, 0),
               InterlockedCompareExchange(&g_acquireTimeouts, 0, 0),
               InterlockedCompareExchange(&g_acquireFrames, 0, 0),
               InterlockedCompareExchange(&g_releaseCalls, 0, 0));
    }
}
/* 冻结开关（v0.6.33）：定义放在匿名命名空间之外，否则引擎侧链接不到 */

bool DupTraceIsFrozen()
{
    return InterlockedCompareExchange(&g_freezeDup, 0, 0) != 0;
}

void DupTraceSetFreeze(bool on)
{
    InterlockedExchange(&g_freezeDup, on ? 1 : 0);
    YZLOGI(L"DXGI 冻结: %s", on ? L"已开启（该进程内的桌面复制将取不到新帧）" : L"已关闭（恢复转发）");
    /* 镜像回宿主（v0.6.33）：现场在 yzt.log 里就能一眼看到冻结何时开、何时关，
       不必再去目标进程的日志目录里翻。 */
    yzhook::SendLogToHost(YZ_LOG_INFO, yz::Format(L"DXGI 冻结: %s", on ? L"已开启" : L"已关闭").c_str());
}

} /* namespace yzhook */
