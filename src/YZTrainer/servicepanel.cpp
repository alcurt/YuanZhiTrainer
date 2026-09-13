//
// 服务/驱动面板：列出与远志相关的服务与内核驱动，支持“临时停止/启动”。
// 只做 sc stop / sc start 等价操作，绝不删除服务、不删除文件。
//
#include "app.h"

#include "yz_log.h"
#include "yz_util.h"

#include <winsvc.h>

#pragma comment(lib, "advapi32.lib")

namespace
{
const int IDC_LIST       = 2101;
const int IDC_BTN_REFRESH = 2102;
const int IDC_BTN_STOP    = 2103;
const int IDC_BTN_START   = 2104;
const int IDC_BTN_CLOSE   = 2105;
const int IDC_LABEL_WARN  = 2106;

const wchar_t* const kPanelClass = L"YZTrainerServicePanel";

struct ServiceInfo
{
    std::wstring name;
    std::wstring display;
    std::wstring imagePath;
    DWORD        state;
};

std::vector<ServiceInfo> g_services;
HWND g_panel = nullptr;
HWND g_list  = nullptr;
UINT  g_panelDpi  = 96;          /* 面板所在显示器的 DPI */
HFONT g_panelFont = nullptr;     /* 按 g_panelDpi 创建 */

const wchar_t* StateText(DWORD state)
{
    switch (state)
    {
    case SERVICE_RUNNING:          return L"运行中";
    case SERVICE_STOPPED:          return L"已停止";
    case SERVICE_START_PENDING:    return L"启动中";
    case SERVICE_STOP_PENDING:     return L"停止中";
    case SERVICE_PAUSED:           return L"已暂停";
    default:                       return L"其它";
    }
}

bool LooksYuanzhiService(const std::wstring& name, const std::wstring& imagePath)
{
    static const wchar_t* kCandidates[] =
    {
        L"exdmirr", L"videfake", L"NdisNetFilter", L"nfndis", L"ExFilter",
        L"DiskFlt", L"UsbFilter", L"ExdDrvGuard", L"GZYZ", L"YZinfo"
    };

    std::wstring lowerName = yz::ToLower(name);
    std::wstring lowerPath = yz::ToLower(imagePath);

    for (size_t i = 0; i < sizeof(kCandidates) / sizeof(kCandidates[0]); i++)
    {
        std::wstring needle = yz::ToLower(kCandidates[i]);
        if (lowerName.find(needle) != std::wstring::npos)
            return true;
        if (!lowerPath.empty() && lowerPath.find(needle) != std::wstring::npos)
            return true;
    }
        return true;
    return false;
}

void RefreshServices()
{
    g_services.clear();

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (scm == nullptr)
    {
        YZLOGE(L"OpenSCManager 失败: %s", yz::Win32ErrorMessage(GetLastError()).c_str());
        return;
    }

    DWORD needed = 0;
    DWORD returned = 0;
    DWORD resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER | SERVICE_WIN32,
                          SERVICE_STATE_ALL, nullptr, 0, &needed, &returned, &resume, nullptr);
    if (needed == 0)
    {
        CloseServiceHandle(scm);
        return;
    }

    std::vector<BYTE> buffer(needed);
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_DRIVER | SERVICE_WIN32,
                               SERVICE_STATE_ALL, buffer.data(), needed, &needed, &returned,
                               &resume, nullptr))
    {
        CloseServiceHandle(scm);
        return;
    }

    ENUM_SERVICE_STATUS_PROCESSW* items =
        reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD i = 0; i < returned; i++)
    {
        ServiceInfo info;
        info.name    = items[i].lpServiceName != nullptr ? items[i].lpServiceName : L"";
        info.display = items[i].lpDisplayName != nullptr ? items[i].lpDisplayName : L"";
        info.state   = items[i].ServiceStatusProcess.dwCurrentState;

        SC_HANDLE svc = OpenServiceW(scm, info.name.c_str(), SERVICE_QUERY_CONFIG);
        if (svc != nullptr)
        {
            DWORD cfgSize = 0;
            QueryServiceConfigW(svc, nullptr, 0, &cfgSize);
            if (cfgSize != 0)
            {
                std::vector<BYTE> cfgBuf(cfgSize);
                QUERY_SERVICE_CONFIGW* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(cfgBuf.data());
                if (QueryServiceConfigW(svc, cfg, cfgSize, &cfgSize) && cfg->lpBinaryPathName != nullptr)
                    info.imagePath = cfg->lpBinaryPathName;
            }
            CloseServiceHandle(svc);
        }

        if (LooksYuanzhiService(info.name, info.imagePath) ||
            (!info.imagePath.empty() && yz::ContainsNoCase(info.imagePath, L"GZYZ")) ||
            (!info.imagePath.empty() && yz::ContainsNoCase(info.imagePath, L"YZinfo")))
        {
            g_services.push_back(info);
        }
    }

    CloseServiceHandle(scm);
}

void FillList()
{
    if (g_list == nullptr)
        return;
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < g_services.size(); i++)
    {
        std::wstring line = yz::Format(L"[%s] %s  (%s)  %s",
                                       StateText(g_services[i].state),
                                       g_services[i].name.c_str(),
                                       g_services[i].display.c_str(),
                                       g_services[i].imagePath.c_str());
        SendMessageW(g_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(line.c_str()));
    }
}

ServiceInfo* SelectedService()
{
    if (g_list == nullptr)
        return nullptr;
    int sel = static_cast<int>(SendMessageW(g_list, LB_GETCURSEL, 0, 0));
    if (sel < 0 || static_cast<size_t>(sel) >= g_services.size())
        return nullptr;
    return &g_services[sel];
}

void ControlSelected(bool start)
{
    ServiceInfo* info = SelectedService();
    if (info == nullptr)
    {
        MessageBoxW(g_panel, L"请先在列表中选择一个服务或驱动。", L"YZTrainer", MB_OK | MB_ICONINFORMATION);
        return;
    }

    if (start && yz::ContainsNoCase(info->name, L"NdisNetFilter"))
    {
        MessageBoxW(g_panel,
                    L"提示：NdisNetFilter 是远志的网卡过滤驱动，停止它可能导致学生端判定为离线并被锁定，"
                    L"也可能影响教师端广播接收。请确认你清楚后果再操作。",
                    L"YZTrainer", MB_OK | MB_ICONWARNING);
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (scm == nullptr)
    {
        MessageBoxW(g_panel, L"无法打开服务管理器（需要管理员权限）。", L"YZTrainer", MB_OK | MB_ICONERROR);
        return;
    }

    SC_HANDLE svc = OpenServiceW(scm, info->name.c_str(),
                                 start ? SERVICE_START : SERVICE_STOP);
    if (svc == nullptr)
    {
        MessageBoxW(g_panel, yz::Win32ErrorMessage(GetLastError()).c_str(), L"YZTrainer", MB_OK | MB_ICONERROR);
        CloseServiceHandle(scm);
        return;
    }

    BOOL ok = FALSE;
    if (start)
    {
        ok = StartServiceW(svc, 0, nullptr);
    }
    else
    {
        SERVICE_STATUS status;
        ok = ControlService(svc, SERVICE_CONTROL_STOP, &status);
    }

    DWORD err = GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    if (ok)
    {
        UiAppendLog(yz::kLogInfo, yz::Format(L"%s服务 %s 成功",
                    start ? L"启动" : L"停止", info->name.c_str()));
    }
    else
    {
        UiAppendLog(yz::kLogWarn, yz::Format(L"%s服务 %s 失败: %s",
                    start ? L"启动" : L"停止", info->name.c_str(),
                    yz::Win32ErrorMessage(err).c_str()));
    }
    RefreshServices();
    FillList();
}

int PanelDp(int value)
{
    return yz::ScaleForDpi(value, g_panelDpi);
}

BOOL CALLBACK CollectPanelChildProc(HWND child, LPARAM lParam)
{
    reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(child);
    return TRUE;
}

/* 按当前 g_panelDpi 创建全部子控件；WM_CREATE 与 WM_DPICHANGED 都走这里。 */
void CreatePanelChildren(HWND hwnd, HINSTANCE hinst)
{
    HFONT font = g_panelFont;

    HWND label = CreateWindowExW(0, L"Static",
        L"与远志相关的服务/驱动（只做临时停止与启动，不删除任何东西）：",
        WS_CHILD | WS_VISIBLE, PanelDp(12), PanelDp(10), PanelDp(700), PanelDp(20),
        hwnd, nullptr, hinst, nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"ListBox", L"",
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | WS_TABSTOP,
                             PanelDp(12), PanelDp(36), PanelDp(700), PanelDp(300), hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)), hinst, nullptr);
    SendMessageW(g_list, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    HWND warn = CreateWindowExW(0, L"Static",
        L"警告：停止 NdisNetFilter 等网络过滤驱动可能触发学生端“离线锁定”，停止显示驱动可能使画面异常。"
        L"重启电脑即可完全恢复。",
        WS_CHILD | WS_VISIBLE, PanelDp(12), PanelDp(344), PanelDp(700), PanelDp(36), hwnd,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LABEL_WARN)), hinst, nullptr);
    SendMessageW(warn, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    const wchar_t* texts[] = { L"刷新", L"停止选中", L"启动选中", L"关闭" };
    const int ids[] = { IDC_BTN_REFRESH, IDC_BTN_STOP, IDC_BTN_START, IDC_BTN_CLOSE };
    int x = PanelDp(12);
    for (int i = 0; i < 4; i++)
    {
        HWND btn = CreateWindowExW(0, L"Button", texts[i],
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   x, PanelDp(386), PanelDp(110), PanelDp(28), hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])), hinst, nullptr);
        SendMessageW(btn, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        x += PanelDp(120);
    }

    RefreshServices();
    FillList();
}

LRESULT CALLBACK PanelProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        CreatePanelChildren(hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)));
        return 0;

    case WM_DPICHANGED:
        {
            g_panelDpi = HIWORD(wParam);
            if (g_panelFont != nullptr)
            {
                DeleteObject(g_panelFont);
                g_panelFont = nullptr;
            }
            g_panelFont = yz::CreateUiFontForDpi(g_panelDpi);

            std::vector<HWND> children;
            EnumChildWindows(hwnd, CollectPanelChildProc, reinterpret_cast<LPARAM>(&children));
            for (size_t i = 0; i < children.size(); i++)
                DestroyWindow(children[i]);
            g_list = nullptr;

            const RECT* want = reinterpret_cast<const RECT*>(lParam);
            if (want != nullptr)
            {
                SetWindowPos(hwnd, nullptr, want->left, want->top,
                             PanelDp(750), PanelDp(470), SWP_NOZORDER | SWP_NOACTIVATE);
            }

            CreatePanelChildren(hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)));
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_BTN_REFRESH:
            RefreshServices();
            FillList();
            break;
        case IDC_BTN_STOP:
            ControlSelected(false);
            break;
        case IDC_BTN_START:
            ControlSelected(true);
            break;
        case IDC_BTN_CLOSE:
            DestroyWindow(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_panel = nullptr;
        g_list  = nullptr;
        if (g_panelFont != nullptr)
        {
            DeleteObject(g_panelFont);
            g_panelFont = nullptr;
        }
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
} /* namespace */

void ServicePanelShow(HWND owner)
{
    HINSTANCE hinst = g_app.hinst;

    if (g_panel != nullptr && IsWindow(g_panel))
    {
        SetForegroundWindow(g_panel);
        return;
    }

    /* DPI：跟随主窗口所在显示器 */
    g_panelDpi = yz::GetWindowDpi(owner != nullptr ? owner : g_app.hwndMain);
    if (g_panelFont != nullptr)
    {
        DeleteObject(g_panelFont);
        g_panelFont = nullptr;
    }
    g_panelFont = yz::CreateUiFontForDpi(g_panelDpi);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = PanelProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = kPanelClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    g_panel = CreateWindowExW(0, kPanelClass, L"YZTrainer - 服务/驱动面板",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              yz::ScaleForDpi(750, g_panelDpi), yz::ScaleForDpi(470, g_panelDpi),
                              owner, nullptr, hinst, nullptr);
    if (g_panel != nullptr)
    {
        ShowWindow(g_panel, SW_SHOW);
        UpdateWindow(g_panel);
    }
}
