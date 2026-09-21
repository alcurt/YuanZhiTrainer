#include "app.h"

#include "yz_log.h"
#include "yz_util.h"

#include "winfix.h"

#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <commctrl.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")

namespace
{
const int IDC_CHK_WINDOWIZE  = 2001;
const int IDC_CHK_UNLOCK     = 2002;
const int IDC_CHK_TOPMOST    = 2003;
const int IDC_CHK_ANTIMON    = 2004;
const int IDC_CHK_BLOCKREMOTE = 2005;
const int IDC_EDIT_PERCENT   = 2006;
const int IDC_BTN_APPLY      = 2007;
const int IDC_STATIC_STATUS  = 2008;
const int IDC_BTN_INJECT     = 2009;
const int IDC_BTN_SERVICE    = 2010;
const int IDC_BTN_DIAG       = 2011;
const int IDC_BTN_LOG        = 2012;
const int IDC_BTN_ABOUT      = 2013;
const int IDC_EDIT_LOG       = 2014;
const int IDC_CHK_EXAMGUARD  = 2015;

const UINT WM_YZ_TRAY        = WM_APP + 10;
const int  IDM_TRAY_SHOW     = 3001;
const int  IDM_TRAY_EXIT     = 3002;
const int  IDM_TRAY_WINDOWIZE = 3003;
const int  IDM_TRAY_UNLOCK   = 3004;
const int  IDM_TRAY_ANTIMON  = 3005;
const int  IDM_TRAY_INJECT   = 3006;

const wchar_t* const kWindowClass = L"YZTrainerMainWnd";
const wchar_t* const kAppTitle    = L"YZTrainer - 远志学生端解控工具";

/* 主窗口风格（创建与 DPI 重排共用，保证两处算出的外框一致） */
const DWORD kMainStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

/* 布局在 96 DPI 下需要的客户区：
   宽 = 12 + 520 + 12
   高 = 12 + 6*26(六行复选框) + 36(百分比行) + 100(状态) + 10 + 26(按钮行) + 8 + 180(日志) + 12 */
const int kClientBaseW = 544;
const int kClientBaseH = 540;

/* 由客户区需求反算窗口外框尺寸。外框尺寸写死会在改布局时漏改
   （曾出现 WM_DPICHANGED 仍用旧尺寸、把日志框底部裁掉），这里统一算。 */
void ComputeOuterSize(UINT dpi, int* outW, int* outH)
{
    RECT rc;
    rc.left   = 0;
    rc.top    = 0;
    rc.right  = yz::ScaleForDpi(kClientBaseW, dpi);
    rc.bottom = yz::ScaleForDpi(kClientBaseH, dpi);

    typedef BOOL (WINAPI *PFN_AdjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static PFN_AdjustWindowRectExForDpi s_forDpi  = nullptr;
    static bool                        s_resolved = false;
    if (!s_resolved)
    {
        s_resolved = true;
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (user32 != nullptr)
        {
            s_forDpi = reinterpret_cast<PFN_AdjustWindowRectExForDpi>(
                GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        }
    }

    if (s_forDpi != nullptr)
        s_forDpi(&rc, kMainStyle, FALSE, 0, dpi);
    else
        AdjustWindowRectEx(&rc, kMainStyle, FALSE, 0);

    *outW = rc.right - rc.left;
    *outH = rc.bottom - rc.top;
}

HWND  g_status  = nullptr;
HWND  g_logEdit = nullptr;
HICON g_icon    = nullptr;
DWORD g_uiThreadId = 0;
NOTIFYICONDATAW g_nid;
bool  g_trayAdded = false;
bool  g_minimizeToTray = true;
UINT  g_dpi     = 96;       /* 当前界面 DPI：所有布局与字体都按它换算 */
HFONT g_font    = nullptr;  /* 按 g_dpi 创建，WM_DPICHANGED 时重建 */

HICON MakeAppIcon(int size)
{
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP color = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    HGDIOBJ old = SelectObject(mem, color);

    RECT r;
    r.left = 0; r.top = 0; r.right = size; r.bottom = size;
    HBRUSH bg = CreateSolidBrush(RGB(24, 64, 120));
    FillRect(mem, &r, bg);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(255, 255, 255));
    DrawTextW(mem, L"YZ", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(mem, old);
    DeleteObject(bg);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    ICONINFO ii;
    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void AppendLogLine(int level, const std::wstring& text)
{
    if (g_logEdit == nullptr)
        return;
    const wchar_t* tag = L"[信息]";
    if (level == yz::kLogError)      tag = L"[错误]";
    else if (level == yz::kLogWarn)  tag = L"[警告]";
    else if (level == yz::kLogDebug) tag = L"[调试]";

    std::wstring line = yz::Format(L"%s %s\r\n", tag, text.c_str());
    int len = GetWindowTextLengthW(g_logEdit);
    SendMessageW(g_logEdit, EM_SETSEL, len, len);
    SendMessageW(g_logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void SyncControls()
{
    if (g_app.hwndMain == nullptr)
        return;
    CheckDlgButton(g_app.hwndMain, IDC_CHK_WINDOWIZE,   (g_app.cfg.flags & YZ_FLAG_WINDOWIZE) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.hwndMain, IDC_CHK_UNLOCK,      (g_app.cfg.flags & YZ_FLAG_INPUT_UNLOCK) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.hwndMain, IDC_CHK_TOPMOST,     (g_app.cfg.flags & YZ_FLAG_TOPMOST) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.hwndMain, IDC_CHK_ANTIMON,     (g_app.cfg.flags & YZ_FLAG_ANTI_MONITOR) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.hwndMain, IDC_CHK_BLOCKREMOTE, (g_app.cfg.flags & YZ_FLAG_BLOCK_REMOTE) ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_app.hwndMain, IDC_CHK_EXAMGUARD,   g_app.cfg.enableExamGuard ? BST_CHECKED : BST_UNCHECKED);
    SetDlgItemTextW(g_app.hwndMain, IDC_EDIT_PERCENT, yz::Format(L"%u", g_app.cfg.windowPercent).c_str());
}

void UpdateStatusText()
{
    if (g_status == nullptr)
        return;

    DWORD clientPid = 0;
    bool connected = IpcIsConnected(&clientPid);
    std::wstring target = g_app.targetPid != 0
        ? yz::Format(L"%u", g_app.targetPid) : L"未发现";
    DWORD fixCount = 0;
    DWORD fixCandidates = 0;
    bool  fixFailed = false;
    winfix::WinFixStats(&fixCount, &fixCandidates, &fixFailed);
    std::wstring text = yz::Format(
        L"连接状态: %s%s\r\n"
        L"目标进程 PID: %s\r\n"
        L"已启用 Hook 数: %u\r\n"
        L"窗口化次数: %u（Hook）/ %u（外部纠正%s）\r\n"
        L"注入次数: %u\r\n"
        L"考试模式: %s",
        connected ? L"已连接" : L"未连接",
        connected ? yz::Format(L"（Hook PID=%u）", clientPid).c_str() : L"",
        target.c_str(),
        g_app.status.hooksInstalled,
        g_app.status.windowizeCount,
        fixCount,
        fixFailed ? L"，有写入失败" : L"",
        g_app.injectCount,
        g_app.examMode ? L"是（全部功能已停用）" : L"否");

    SetWindowTextW(g_status, text.c_str());
}

void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SHOW, L"显示主界面");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (CmdGetFlag(YZ_FLAG_WINDOWIZE) ? MF_CHECKED : 0), IDM_TRAY_WINDOWIZE, L"广播窗口化");
    AppendMenuW(menu, MF_STRING | (CmdGetFlag(YZ_FLAG_INPUT_UNLOCK) ? MF_CHECKED : 0), IDM_TRAY_UNLOCK, L"解除键鼠锁定");
    AppendMenuW(menu, MF_STRING | (CmdGetFlag(YZ_FLAG_ANTI_MONITOR) ? MF_CHECKED : 0), IDM_TRAY_ANTIMON, L"防监视（冻结画面）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_INJECT, L"立即注入");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"退出");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

void AddTrayIcon(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hwnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_YZ_TRAY;
    g_nid.hIcon            = g_icon;
    wcsncpy_s(g_nid.szTip, sizeof(g_nid.szTip) / sizeof(wchar_t), kAppTitle, _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &g_nid))
        g_trayAdded = true;
}

void RemoveTrayIcon()
{
    if (g_trayAdded)
    {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayAdded = false;
    }
}

void Balloon(const wchar_t* title, const wchar_t* text)
{
    if (!g_trayAdded)
        return;
    NOTIFYICONDATAW nid = g_nid;
    nid.uFlags     = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, sizeof(nid.szInfoTitle) / sizeof(wchar_t), title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, sizeof(nid.szInfo) / sizeof(wchar_t), text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* 第一次隐藏到托盘时给一条气泡提示（照 JiYuTrainer 的做法），
   免得用户以为程序被关掉了；只提示一次，反复最小化时不打扰。 */
void NotifyHiddenToTray()
{
    static bool s_notified = false;
    if (s_notified)
        return;
    s_notified = true;
    Balloon(L"YZTrainer 提示", L"窗口已隐藏到托盘：双击托盘图标显示主界面，右键打开菜单");
}

void OnCommandWord(HWND hwnd, int id)
{
    switch (id)
    {
    case IDC_CHK_WINDOWIZE:
        CmdSetFlag(YZ_FLAG_WINDOWIZE, IsDlgButtonChecked(hwnd, IDC_CHK_WINDOWIZE) == BST_CHECKED);
        break;
    case IDC_CHK_UNLOCK:
        CmdSetFlag(YZ_FLAG_INPUT_UNLOCK, IsDlgButtonChecked(hwnd, IDC_CHK_UNLOCK) == BST_CHECKED);
        break;
    case IDC_CHK_TOPMOST:
        CmdSetFlag(YZ_FLAG_TOPMOST, IsDlgButtonChecked(hwnd, IDC_CHK_TOPMOST) == BST_CHECKED);
        break;
    case IDC_CHK_ANTIMON:
        CmdSetFlag(YZ_FLAG_ANTI_MONITOR, IsDlgButtonChecked(hwnd, IDC_CHK_ANTIMON) == BST_CHECKED);
        break;
    case IDC_CHK_BLOCKREMOTE:
        CmdSetFlag(YZ_FLAG_BLOCK_REMOTE, IsDlgButtonChecked(hwnd, IDC_CHK_BLOCKREMOTE) == BST_CHECKED);
        break;
    case IDC_CHK_EXAMGUARD:
        {
            g_app.cfg.enableExamGuard = (IsDlgButtonChecked(hwnd, IDC_CHK_EXAMGUARD) == BST_CHECKED);
            if (g_app.cfg.enableExamGuard)
                g_app.cfg.flags |= YZ_CFG_EXAM_GUARD;
            else
                g_app.cfg.flags &= ~YZ_CFG_EXAM_GUARD;
            ConfigSave(g_app.cfg, g_app.iniPath);
            IpcSendConfig();
            SyncControls();
            UiAppendLog(yz::kLogInfo, g_app.cfg.enableExamGuard
                        ? L"考试模式守护已开启（仅强信号熔断）"
                        : L"考试模式守护已关闭（跳过考试检测）");
        }
        break;
    case IDC_BTN_APPLY:
        {
            wchar_t buf[32] = {0};
            GetDlgItemTextW(hwnd, IDC_EDIT_PERCENT, buf, 32);
            int percent = _wtoi(buf);
            if (percent < 20) percent = 20;
            if (percent > 100) percent = 100;
            g_app.cfg.windowPercent = static_cast<DWORD>(percent);
            ConfigSave(g_app.cfg, g_app.iniPath);
            IpcSendConfig();
            SyncControls();
            UiAppendLog(yz::kLogInfo, yz::Format(L"窗口宽度设为 %d%%", percent));
        }
        break;
    case IDC_BTN_INJECT:
        CmdInjectNow();
        break;
    case IDC_BTN_SERVICE:
        CmdShowServicePanel();
        break;
    case IDC_BTN_DIAG:
        CmdExportDiag();
        break;
    case IDC_BTN_LOG:
        CmdOpenLogFolder();
        break;
    case IDC_BTN_ABOUT:
        CmdShowAbout();
        break;
    default:
        break;
    }
}

int Dp(int value)
{
    return yz::ScaleForDpi(value, g_dpi);
}

BOOL CALLBACK CollectChildProc(HWND child, LPARAM lParam)
{
    reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(child);
    return TRUE;
}

/* 按当前 g_dpi 创建全部子控件。WM_CREATE 与 WM_DPICHANGED 都走这里，
   所以每一处尺寸都必须经过 Dp()，不能写字面像素值。 */
void CreateChildren(HWND hwnd, HINSTANCE hinst)
{
    HFONT font = g_font;
    int y = Dp(12);

    const wchar_t* items[] = {
        L"全屏广播窗口化（把全屏广播变成可自由操作的窗口）",
        L"解除键鼠锁定（拦截远志的键盘/鼠标钩子与热键屏蔽）",
        L"广播窗口保持置顶",
        L"防监视（冻结教师端看到的画面，默认关闭）",
        L"拦截教师端遥控输入（默认关闭，会影响老师远程协助）",
        L"考试模式守护（仅强信号熔断，默认开启）"
    };
    const int ids[] = { IDC_CHK_WINDOWIZE, IDC_CHK_UNLOCK, IDC_CHK_TOPMOST,
                        IDC_CHK_ANTIMON, IDC_CHK_BLOCKREMOTE, IDC_CHK_EXAMGUARD };

    for (int i = 0; i < 6; i++)
    {
        HWND chk = CreateWindowExW(0, L"Button", items[i],
                                   WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                   Dp(12), y, Dp(520), Dp(22), hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])), hinst, nullptr);
        SendMessageW(chk, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        y += Dp(26);
    }

    HWND label = CreateWindowExW(0, L"Static", L"广播窗口宽度（占屏幕百分比）:",
                                 WS_CHILD | WS_VISIBLE, Dp(12), y + Dp(4), Dp(220), Dp(20),
                                 hwnd, nullptr, hinst, nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"60",
                                WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                                Dp(240), y, Dp(60), Dp(24), hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_PERCENT)), hinst, nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    HWND apply = CreateWindowExW(0, L"Button", L"应用",
                                 WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 Dp(308), y, Dp(60), Dp(24), hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BTN_APPLY)), hinst, nullptr);
    SendMessageW(apply, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    y += Dp(36);

    g_status = CreateWindowExW(WS_EX_CLIENTEDGE, L"Static", L"",
                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                               Dp(12), y, Dp(520), Dp(100), hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATIC_STATUS)), hinst, nullptr);
    SendMessageW(g_status, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    y += Dp(110);

    const wchar_t* btnText[] = { L"立即注入", L"服务面板", L"导出诊断包", L"打开日志目录", L"关于" };
    const int btnId[] = { IDC_BTN_INJECT, IDC_BTN_SERVICE, IDC_BTN_DIAG, IDC_BTN_LOG, IDC_BTN_ABOUT };
    int bx = Dp(12);
    for (int i = 0; i < 5; i++)
    {
        HWND btn = CreateWindowExW(0, L"Button", btnText[i],
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   bx, y, Dp(100), Dp(26), hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(btnId[i])), hinst, nullptr);
        SendMessageW(btn, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        bx += Dp(106);
    }
    y += Dp(34);

    g_logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE |
                                    ES_AUTOVSCROLL | ES_READONLY,
                                Dp(12), y, Dp(520), Dp(180), hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EDIT_LOG)), hinst, nullptr);
    SendMessageW(g_logEdit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    SyncControls();
    UpdateStatusText();
}

/* 定义在 WndProc 之后，这里前置声明，避免 C3861。 */
int TrayMenuCommand(int id);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        /* 必须先登记主窗口句柄：CreateChildren → SyncControls() 依赖 g_app.hwndMain，
           否则冷启动时所有复选框会停在“未勾选”，直到第一条状态消息才刷新成真实状态。 */
        g_app.hwndMain = hwnd;
        CreateChildren(hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)));
        return 0;

    case WM_DPICHANGED:
        {
            /* 换到缩放不同的显示器：按新 DPI 重建字体与全部子控件。
               日志框内容先取出来，重建完再灌回去，避免历史日志被清空。 */
            g_dpi = HIWORD(wParam);
            if (g_font != nullptr)
            {
                DeleteObject(g_font);
                g_font = nullptr;
            }
            g_font = yz::CreateUiFontForDpi(g_dpi);

            std::wstring logText;
            if (g_logEdit != nullptr)
            {
                const int len = GetWindowTextLengthW(g_logEdit);
                if (len > 0)
                {
                    std::vector<wchar_t> buf(static_cast<size_t>(len) + 1, L'\0');
                    GetWindowTextW(g_logEdit, buf.data(), len + 1);
                    logText.assign(buf.data());
                }
            }

            std::vector<HWND> children;
            EnumChildWindows(hwnd, CollectChildProc, reinterpret_cast<LPARAM>(&children));
            for (size_t i = 0; i < children.size(); i++)
                DestroyWindow(children[i]);
            g_status  = nullptr;
            g_logEdit = nullptr;

            const RECT* want = reinterpret_cast<const RECT*>(lParam);
            if (want != nullptr)
            {
                int outerW = 0;
                int outerH = 0;
                ComputeOuterSize(g_dpi, &outerW, &outerH);
                SetWindowPos(hwnd, nullptr, want->left, want->top,
                             outerW, outerH, SWP_NOZORDER | SWP_NOACTIVATE);
            }

            CreateChildren(hwnd, reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE)));
            if (!logText.empty() && g_logEdit != nullptr)
                SetWindowTextW(g_logEdit, logText.c_str());

            UiAppendLog(yz::kLogInfo, yz::Format(L"显示缩放变化，界面已按 %u%% 重新布局", g_dpi * 100 / 96));
        }
        return 0;

    case WM_COMMAND:
        {
            int id = LOWORD(wParam);
            if (id >= IDM_TRAY_SHOW && id <= IDM_TRAY_INJECT)
                return TrayMenuCommand(id);
            if (HIWORD(wParam) == BN_CLICKED || HIWORD(wParam) == EN_CHANGE)
                OnCommandWord(hwnd, id);
        }
        return 0;

    case WM_YZ_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK)
        {
            /* 双击托盘图标 = 显示主界面（与隐藏提示里说的一致） */
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        }
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP)
            ShowTrayMenu(hwnd);
        return 0;

    case WM_YZ_STATUS:
        UpdateStatusText();
        SyncControls();
        return 0;

    case WM_YZ_LOG:
        {
            std::wstring* text = reinterpret_cast<std::wstring*>(lParam);
            if (text != nullptr)
            {
                AppendLogLine(static_cast<int>(wParam), *text);
                delete text;
            }
        }
        return 0;

    case WM_YZ_EXAM:
        UpdateStatusText();
        return 0;

    case WM_HOTKEY:
        switch (static_cast<int>(wParam))
        {
        case ID_HOTKEY_WINDOWIZE: CmdToggleFlag(YZ_FLAG_WINDOWIZE); break;
        case ID_HOTKEY_UNLOCK:    CmdToggleFlag(YZ_FLAG_INPUT_UNLOCK); break;
        case ID_HOTKEY_ANTIMON:   CmdToggleFlag(YZ_FLAG_ANTI_MONITOR); break;
        case ID_HOTKEY_SHOWUI:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            break;
        default:
            break;
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE && g_minimizeToTray)
        {
            ShowWindow(hwnd, SW_HIDE);
            NotifyHiddenToTray();
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        NotifyHiddenToTray();
        return 0;

    case WM_DESTROY:
        RemoveTrayIcon();
        if (g_font != nullptr)
        {
            DeleteObject(g_font);
            g_font = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int TrayMenuCommand(int id)
{
    switch (id)
    {
    case IDM_TRAY_SHOW:
        ShowWindow(g_app.hwndMain, SW_SHOW);
        SetForegroundWindow(g_app.hwndMain);
        return 0;
    case IDM_TRAY_EXIT:
        ShowWindow(g_app.hwndMain, SW_HIDE);
        DestroyWindow(g_app.hwndMain);
        return 0;
    case IDM_TRAY_WINDOWIZE: CmdToggleFlag(YZ_FLAG_WINDOWIZE); break;
    case IDM_TRAY_UNLOCK:    CmdToggleFlag(YZ_FLAG_INPUT_UNLOCK); break;
    case IDM_TRAY_ANTIMON:   CmdToggleFlag(YZ_FLAG_ANTI_MONITOR); break;
    case IDM_TRAY_INJECT:    CmdInjectNow(); break;
    default:
        break;
    }
    return 0;
}
} /* namespace */

HWND UiInit(HINSTANCE hinst)
{
    g_uiThreadId = GetCurrentThreadId();
    g_app.hinst  = hinst;

    /* 高 DPI：清单里声明了 PerMonitorV2，这里先按系统 DPI 建字体与尺寸；
       窗口若落到缩放不同的显示器上，再由 WM_DPICHANGED 重新布局。 */
    g_dpi = yz::GetWindowDpi(nullptr);
    if (g_font != nullptr)
        DeleteObject(g_font);
    g_font = yz::CreateUiFontForDpi(g_dpi);
    YZLOGI(L"界面 DPI = %u (%u%%)", g_dpi, g_dpi * 100 / 96);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hinst;
    wc.lpszClassName = kWindowClass;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hIcon         = MakeAppIcon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm       = MakeAppIcon(GetSystemMetrics(SM_CXSMICON));
    if (!RegisterClassExW(&wc))
    {
        YZLOGE(L"RegisterClassEx 失败: %s", yz::Win32ErrorMessage(GetLastError()).c_str());
        return nullptr;
    }
    g_icon = wc.hIcon;

    int outerW = 0;
    int outerH = 0;
    ComputeOuterSize(g_dpi, &outerW, &outerH);

    HWND hwnd = CreateWindowExW(0, kWindowClass, kAppTitle, kMainStyle,
                                CW_USEDEFAULT, CW_USEDEFAULT, outerW, outerH,
                                nullptr, nullptr, hinst, nullptr);
    if (hwnd == nullptr)
    {
        YZLOGE(L"CreateWindowEx 失败: %s", yz::Win32ErrorMessage(GetLastError()).c_str());
        return nullptr;
    }

    g_app.hwndMain = hwnd;
    AddTrayIcon(hwnd);

    RegisterHotKey(hwnd, ID_HOTKEY_WINDOWIZE, MOD_CONTROL | MOD_ALT, VK_F9);
    RegisterHotKey(hwnd, ID_HOTKEY_UNLOCK,    MOD_CONTROL | MOD_ALT, VK_F10);
    RegisterHotKey(hwnd, ID_HOTKEY_ANTIMON,   MOD_CONTROL | MOD_ALT, VK_F11);
    RegisterHotKey(hwnd, ID_HOTKEY_SHOWUI,    MOD_CONTROL | MOD_ALT, VK_F12);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

void UiShutdown()
{
    if (g_app.hwndMain != nullptr)
    {
        UnregisterHotKey(g_app.hwndMain, ID_HOTKEY_WINDOWIZE);
        UnregisterHotKey(g_app.hwndMain, ID_HOTKEY_UNLOCK);
        UnregisterHotKey(g_app.hwndMain, ID_HOTKEY_ANTIMON);
        UnregisterHotKey(g_app.hwndMain, ID_HOTKEY_SHOWUI);
    }
    RemoveTrayIcon();
}

void UiUpdateStatus()
{
    UpdateStatusText();
}

void UiAppendLog(int level, const std::wstring& text)
{
    if (g_app.hwndMain == nullptr)
        return;
    if (GetCurrentThreadId() != g_uiThreadId)
    {
        std::wstring* copy = new std::wstring(text);
        PostMessageW(g_app.hwndMain, WM_YZ_LOG, static_cast<WPARAM>(level),
                     reinterpret_cast<LPARAM>(copy));
        return;
    }
    AppendLogLine(level, text);
}

void UiSyncControls()
{
    SyncControls();
    UpdateStatusText();
}

void UiNotifyExam(bool exam)
{
    if (exam)
    {
        UiAppendLog(yz::kLogWarn, L"检测到考试/测验模式，已停用全部功能");
        Balloon(L"YZTrainer", L"检测到考试模式，全部功能已自动停用");
    }
    else
    {
        UiAppendLog(yz::kLogInfo, L"考试模式结束，功能按当前配置恢复");
    }
}

void CmdSetFlag(DWORD flag, bool on)
{
    if (on)
        g_app.cfg.flags |= flag;
    else
        g_app.cfg.flags &= ~flag;

    ConfigSave(g_app.cfg, g_app.iniPath);

    DWORD opcode = 0;
    if (flag == YZ_FLAG_WINDOWIZE)      opcode = YZ_CMD_SET_WINDOW_MODE;
    if (flag == YZ_FLAG_INPUT_UNLOCK)   opcode = YZ_CMD_SET_INPUT_UNLOCK;
    if (flag == YZ_FLAG_ANTI_MONITOR)   opcode = YZ_CMD_SET_ANTI_MONITOR;
    if (flag == YZ_FLAG_BLOCK_REMOTE)   opcode = YZ_CMD_SET_BLOCK_REMOTE;

    if (opcode != 0)
        IpcSendFlagCommand(opcode, on);
    else
        IpcSendConfig();

    UiSyncControls();
}

void CmdToggleFlag(DWORD flag)
{
    CmdSetFlag(flag, !CmdGetFlag(flag));
}

bool CmdGetFlag(DWORD flag)
{
    return (g_app.cfg.flags & flag) != 0;
}

void CmdInjectNow()
{
    g_app.lastInjectTick = 0;
    WatchdogTick();
}

void CmdShowServicePanel()
{
    ServicePanelShow(g_app.hwndMain);
}

void CmdExportDiag()
{
    ExportDiagnostics(g_app.hwndMain);
}

void CmdOpenLogFolder()
{
    std::wstring dir = yz::LogDir();
    yz::EnsureDirectory(dir);
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void CmdShowAbout()
{
    MessageBoxW(g_app.hwndMain,
                L"YZTrainer " YZ_VERSION_STR L"\n\n"
                L"用途：在教师全屏广播时把广播画面改成可自由操作的窗口，"
                L"并解除远志学生端对本机的键鼠封锁。\n"
                L"窗口化有两条路：注入成功走进程内 Hook；注入被拒（VirtualAllocEx 0x5）时"
                L"由主程序跨进程改窗口样式兜底（ExternalWindowFix，默认开）。\n"
                L"原则：不修改远志文件、不卸载驱动、不干扰教师端；"
                L"检测到考试强信号（独立考试进程 ExamDlg.exe、考试对话框组件或"
                L"可见考试窗口）时全部功能自动停用；Exam.ads / ClassQuiz.ads 属于"
                L"学生端启动就绪模块，只会记日志、不再触发熔断。\n\n"
                L"热键：Ctrl+Alt+F9 广播窗口化 / F10 键鼠解锁 / F11 防监视 / F12 显示界面",
                L"关于 YZTrainer", MB_OK | MB_ICONINFORMATION);
}


