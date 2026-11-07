//
// 软键盘（v0.6.73）。
//
// 现场问题：教师端"锁键盘"之后，物理按键一个都进不来。两次现场日志全量筛过——Yistart 的用户态里
// 没有任何输入 API（无 keybd_event / SendInput / BlockInput），所以锁大概率在驱动层，用户态解不掉。
//
// 但**消息级注入不过键盘驱动栈**：把 WM_KEYDOWN + WM_CHAR 直接 PostMessage 给焦点窗口，字符照样进去。
// 这就是本文件存在的意义——键盘锁着，鼠标还能动，那就用鼠标点字。
//
// 几个实现要点（踩过的坑）：
//   * WS_EX_NOACTIVATE + WM_MOUSEACTIVATE 返回 MA_NOACTIVATE：点按钮时**不能**把前台窗口抢走，
//     否则字会打到自己身上；
//   * 目标窗口在点击那一刻用 GetForegroundWindow() 取；若前台是我们自己（主窗口/软键盘），
//     退回上一次记下的目标；
//   * 只投递消息，不调用 SendInput —— 后者会重新走进被锁的输入栈。
//
#include "softkbd.h"

#include "app.h"
#include "yz_log.h"

#include <string>

namespace softkbd {
namespace {

const wchar_t* kClass = L"YZSoftKbd";

HWND g_kbd = nullptr;
HWND g_owner = nullptr;
HWND g_target = nullptr;
bool g_shift = false;
bool g_ctrl = false;
bool g_alt = false;

struct KeyDef
{
    const wchar_t* label;   /* 按钮上的字 */
    WORD           vk;      /* 虚拟键码，0 表示纯字符键 */
    wchar_t        ch;      /* 普通状态投递的字符，0 表示不投 WM_CHAR */
    wchar_t        chShift; /* Shift 状态下投递的字符 */
    int            width;   /* 占几个标准按钮宽 */
    bool           isMod;   /* 修饰键：只切换状态，不投递 */
};

/* 布局：五排。宽单位以 34 像素为准，长键按倍数伸。 */
const KeyDef kRow0[] = {
    { L"`", 0x00C0, L'`', L'~', 1, false }, { L"1", 0x0031, L'1', L'!', 1, false },
    { L"2", 0x0032, L'2', L'@', 1, false }, { L"3", 0x0033, L'3', L'#', 1, false },
    { L"4", 0x0034, L'4', L'$', 1, false }, { L"5", 0x0035, L'5', L'%', 1, false },
    { L"6", 0x0036, L'6', L'^', 1, false }, { L"7", 0x0037, L'7', L'&', 1, false },
    { L"8", 0x0038, L'8', L'*', 1, false }, { L"9", 0x0039, L'9', L'(', 1, false },
    { L"0", 0x0030, L'0', L')', 1, false }, { L"-", 0x00BD, L'-', L'_', 1, false },
    { L"=", 0x00BB, L'=', L'+', 1, false }, { L"退格", 0x0008, 0x0008, 0x0008, 2, false },
};
const KeyDef kRow1[] = {
    { L"Tab", 0x0009, 0x0009, 0x0009, 2, false },
    { L"Q", 0x0051, L'q', L'Q', 1, false }, { L"W", 0x0057, L'w', L'W', 1, false },
    { L"E", 0x0045, L'e', L'E', 1, false }, { L"R", 0x0052, L'r', L'R', 1, false },
    { L"T", 0x0054, L't', L'T', 1, false }, { L"Y", 0x0059, L'y', L'Y', 1, false },
    { L"U", 0x0055, L'u', L'U', 1, false }, { L"I", 0x0049, L'i', L'I', 1, false },
    { L"O", 0x004F, L'o', L'O', 1, false }, { L"P", 0x0050, L'p', L'P', 1, false },
    { L"[", 0x00DB, L'[', L'{', 1, false }, { L"]", 0x00DD, L']', L'}', 1, false },
    { L"\\", 0x00DC, L'\\', L'|', 1, false },
};
const KeyDef kRow2[] = {
    { L"Caps", 0x0014, 0, 0, 2, true },
    { L"A", 0x0041, L'a', L'A', 1, false }, { L"S", 0x0053, L's', L'S', 1, false },
    { L"D", 0x0044, L'd', L'D', 1, false }, { L"F", 0x0046, L'f', L'F', 1, false },
    { L"G", 0x0047, L'g', L'G', 1, false }, { L"H", 0x0048, L'h', L'H', 1, false },
    { L"J", 0x004A, L'j', L'J', 1, false }, { L"K", 0x004B, L'k', L'K', 1, false },
    { L"L", 0x004C, L'l', L'L', 1, false }, { L";", 0x00BA, L';', L':', 1, false },
    { L"'", 0x00DE, L'\'', L'"', 1, false },
    { L"回车", 0x000D, L'\r', L'\r', 3, false },
};
const KeyDef kRow3[] = {
    { L"Shift", 0x0010, 0, 0, 2, true },
    { L"Z", 0x005A, L'z', L'Z', 1, false }, { L"X", 0x0058, L'x', L'X', 1, false },
    { L"C", 0x0043, L'c', L'C', 1, false }, { L"V", 0x0056, L'v', L'V', 1, false },
    { L"B", 0x0042, L'b', L'B', 1, false }, { L"N", 0x004E, L'n', L'N', 1, false },
    { L"M", 0x004D, L'm', L'M', 1, false }, { L",", 0x00BC, L',', L'<', 1, false },
    { L".", 0x00BE, L'.', L'>', 1, false }, { L"/", 0x00BF, L'/', L'?', 1, false },
    { L"上", 0x0026, 0, 0, 1, false }, { L"下", 0x0028, 0, 0, 1, false },
};
const KeyDef kRow4[] = {
    { L"Ctrl", 0x0011, 0, 0, 2, true },
    { L"Alt", 0x0012, 0, 0, 2, true },
    { L"空格", 0x0020, L' ', L' ', 6, false },
    { L"Esc", 0x001B, 0x001B, 0x001B, 2, false },
    { L"左", 0x0025, 0, 0, 1, false },
    { L"右", 0x0027, 0, 0, 1, false },
    { L"Home", 0x0024, 0, 0, 1, false },
    { L"End", 0x0023, 0, 0, 1, false },
};

const KeyDef* const kRows[] = { kRow0, kRow1, kRow2, kRow3, kRow4 };
const int kRowCount = 5;
const int kKeyBaseId = 5000;
const int kUnit = 34;      /* 像素，100% 缩放下的标准键宽 */
const int kGap = 3;

struct KeySlot
{
    const KeyDef* def;
    int           id;
};
KeySlot g_slots[64];
int g_slotCount = 0;

int DpiScale(int px)
{
    HDC dc = GetDC(nullptr);
    int dpi = 96;
    if (dc != nullptr)
    {
        dpi = GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
    }
    if (dpi < 96)
        dpi = 96;
    return px * dpi / 96;
}

void PostKey(const KeyDef* d)
{
    HWND fg = GetForegroundWindow();
    if (fg != nullptr && fg != g_kbd && fg != g_owner)
        g_target = fg;
    HWND target = (g_target != nullptr && IsWindow(g_target)) ? g_target : fg;
    if (target == nullptr)
    {
        YZLOGW(L"软键盘: 没有可投递的目标窗口（先点一下要输入的窗口）");
        return;
    }

    if (d->isMod)
    {
        if (d->vk == 0x0010)      g_shift = !g_shift;
        else if (d->vk == 0x0011) g_ctrl = !g_ctrl;
        else if (d->vk == 0x0012) g_alt = !g_alt;
        else if (d->vk == 0x0014) g_shift = !g_shift;   /* Caps 简化为 Shift */
        YZLOGI(L"软键盘: 修饰键 Shift=%d Ctrl=%d Alt=%d", g_shift ? 1 : 0, g_ctrl ? 1 : 0, g_alt ? 1 : 0);
        return;
    }

    const wchar_t ch = (g_shift && d->chShift != 0) ? d->chShift : d->ch;
    const WORD vk = d->vk;
    const LPARAM lDown = (g_shift || g_ctrl || g_alt) ? 0x20000001 : 0x00000001;
    PostMessageW(target, WM_KEYDOWN, static_cast<WPARAM>(vk), lDown);
    if (ch != 0)
        PostMessageW(target, WM_CHAR, static_cast<WPARAM>(ch), lDown);
    PostMessageW(target, WM_KEYUP, static_cast<WPARAM>(vk), 0xC0000001);
    YZLOGI(L"软键盘: 已向窗口 %p 投递 %s（vk=0x%02X 字符=0x%04X）",
           target, d->label, static_cast<unsigned>(vk), static_cast<unsigned>(ch));
}

void CreateButtons(HWND hwnd)
{
    g_slotCount = 0;
    int y = kGap;
    for (int r = 0; r < kRowCount; ++r)
    {
        const KeyDef* row = kRows[r];
        int count = 0;
        while (row[count].label != nullptr)
            ++count;
        int x = kGap;
        for (int i = 0; i < count && g_slotCount < 64; ++i)
        {
            const int w = kUnit * row[i].width + kGap * (row[i].width - 1);
            const int id = kKeyBaseId + g_slotCount;
            CreateWindowExW(0, L"Button", row[i].label,
                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            x, y, w, kUnit, hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
            g_slots[g_slotCount].def = &row[i];
            g_slots[g_slotCount].id  = id;
            ++g_slotCount;
            x += w + kGap;
        }
        y += kUnit + kGap;
    }
}

LRESULT CALLBACK KbdProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        CreateButtons(hwnd);
        return 0;

    case WM_MOUSEACTIVATE:
        /* 关键：点软键盘不抢前台，否则字符会打回自己身上 */
        return MA_NOACTIVATE;

    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        for (int i = 0; i < g_slotCount; ++i)
        {
            if (g_slots[i].id == id)
            {
                PostKey(g_slots[i].def);
                return 0;
            }
        }
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool EnsureClass()
{
    static bool s_registered = false;
    if (s_registered)
        return true;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = KbdProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClass;
    if (RegisterClassExW(&wc) == 0)
    {
        YZLOGW(L"软键盘: RegisterClassEx 失败 err=%u", GetLastError());
        return false;
    }
    s_registered = true;
    return true;
}

}  // namespace

void Toggle(HWND hwndOwner)
{
    g_owner = hwndOwner;
    if (g_kbd != nullptr && IsWindowVisible(g_kbd))
    {
        ShowWindow(g_kbd, SW_HIDE);
        YZLOGI(L"软键盘: 已隐藏");
        return;
    }
    if (!EnsureClass())
        return;

    if (g_kbd == nullptr || !IsWindow(g_kbd))
    {
        /* 尺寸按 14 个标准键宽 + 边距算 */
        const int w = kUnit * 14 + kGap * 15;
        const int h = kUnit * 5 + kGap * 6;
        g_kbd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                kClass, L"YZTrainer 软键盘（键盘被锁时用鼠标点）",
                                WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (g_kbd == nullptr)
        {
            YZLOGW(L"软键盘: CreateWindowEx 失败 err=%u", GetLastError());
            return;
        }
        g_target = GetForegroundWindow();
    }

    /* 贴近屏幕底部居中 */
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT rc;
    GetWindowRect(g_kbd, &rc);
    const int ww = rc.right - rc.left;
    const int wh = rc.bottom - rc.top;
    SetWindowPos(g_kbd, HWND_TOPMOST,
                 work.left + (work.right - work.left - ww) / 2,
                 work.bottom - wh - DpiScale(48), 0, 0,
                 SWP_NOSIZE | SWP_SHOWWINDOW);
    YZLOGI(L"软键盘: 已显示（目标窗口 %p；消息级注入，不经过输入栈）", g_target);
}

void Shutdown()
{
    if (g_kbd != nullptr && IsWindow(g_kbd))
        DestroyWindow(g_kbd);
    g_kbd = nullptr;
    g_target = nullptr;
}

bool IsVisible()
{
    return g_kbd != nullptr && IsWindow(g_kbd) && IsWindowVisible(g_kbd) != FALSE;
}

}  // namespace softkbd
