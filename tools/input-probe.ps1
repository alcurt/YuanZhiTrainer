# YZTrainer 输入路径探针（v0.6.40 起随现场清单使用）
#
# 目的：现场遇到"键鼠被远志屏蔽"时，用 10 秒量出**到底是哪一层**被拦，再决定对策。
#       范围严格限定在用户态：不写内核驱动、不碰厂商驱动文件（项目硬边界）。
#
# 三个试验：
#   A. SendInput    —— 走标准输入队列（最底层用户态路径）。过不了 ⇒ 输入队列前就被拦了。
#   B. PostMessage  —— 把按键消息直接投给指定窗口（绕过输入队列，只对认消息的程序有效）。
#   C. 低层钩子计数 —— 本脚本自己装 WH_KEYBOARD_LL，看**物理按键是否还被派发到会话**。
#                      收不到任何计数 ⇒ 更可能是设备级（驱动）拦截，如实记录、不越界。
#
# 用法（管理员 PowerShell；5.1 与 pwsh 7 都能跑）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\input-probe.ps1
#   可选参数：-TargetProc Yistart.exe  -HoldSeconds 8
#
# 输出：目录下 input-probe-<时间戳>.txt（把 A/B 的人工观察与 C 的自动计数都写进去，带回分析）

param(
    [string]$TargetProc = '',
    [int]$HoldSeconds = 8,
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrEmpty($OutDir)) { $OutDir = (Get-Location).Path }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outFile = Join-Path $OutDir ("input-probe-{0}.txt" -f $stamp)

$log = New-Object System.Collections.Generic.List[string]
function Say([string]$s) { $log.Add($s); Write-Host $s }

Say ("==== YZTrainer 输入路径探针 {0} ====" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Say ("管理员: " + ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
Say ("目标进程参数: " + $(if ($TargetProc) { $TargetProc } else { '(未指定，默认前台窗口)' }))
Say ''

# ---------------------------------------------------------------- 公共 P/Invoke
if (-not ('YzInputProbe' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class YzInputProbe
{
    // ---- SendInput ----
    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)]
    public struct HARDWAREINPUT { public uint uMsg; public ushort wParamL; public ushort wParamH; }
    [StructLayout(LayoutKind.Explicit)]
    public struct INPUTUNION { [FieldOffset(0)] public KEYBDINPUT ki; [FieldOffset(0)] public MOUSEINPUT mi; [FieldOffset(0)] public HARDWAREINPUT hi; }
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public INPUTUNION u; }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder s, int n);

    public const uint INPUT_KEYBOARD = 1;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public const uint KEYEVENTF_UNICODE = 0x0004;
    public const uint WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102;

    // 用 KEYEVENTF_UNICODE 打一段文字（不依赖键盘布局，最"干净"的 SendInput 路径）
    public static uint SendUnicodeText(string text)
    {
        List<INPUT> list = new List<INPUT>();
        foreach (char ch in text)
        {
            INPUT down = new INPUT(); down.type = INPUT_KEYBOARD;
            down.u.ki.wScan = ch; down.u.ki.dwFlags = KEYEVENTF_UNICODE;
            INPUT up = new INPUT(); up.type = INPUT_KEYBOARD;
            up.u.ki.wScan = ch; up.u.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            list.Add(down); list.Add(up);
        }
        return SendInput((uint)list.Count, list.ToArray(), Marshal.SizeOf(typeof(INPUT)));
    }

    // 鼠标相对移动一小段（标准输入队列）
    public static uint SendMouseMove(int dx, int dy)
    {
        INPUT inp = new INPUT(); inp.type = 0; // MOUSE
        inp.u.mi.dx = dx; inp.u.mi.dy = dy; inp.u.mi.dwFlags = 0x0001; // MOUSEEVENTF_MOVE
        INPUT[] arr = new INPUT[] { inp };
        return SendInput(1, arr, Marshal.SizeOf(typeof(INPUT)));
    }

    public static string DescribeWindow(IntPtr hwnd)
    {
        if (hwnd == IntPtr.Zero) return "(无窗口)";
        StringBuilder t = new StringBuilder(256), c = new StringBuilder(256);
        GetWindowTextW(hwnd, t, t.Capacity);
        GetClassNameW(hwnd, c, c.Capacity);
        uint pid; GetWindowThreadProcessId(hwnd, out pid);
        return string.Format("hwnd=0x{0:X} pid={1} class={2} title={3}", hwnd.ToInt64(), pid, c, t);
    }

    // ---- 低层键盘钩子：数物理按键还有没有被派发到会话 ----
    public delegate IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SetWindowsHookExW(int idHook, HookProc lpfn, IntPtr hMod, uint dwThreadId);
    [DllImport("user32.dll", SetLastError = true)] public static extern bool UnhookWindowsHookEx(IntPtr hhk);
    [DllImport("user32.dll")] public static extern IntPtr CallNextHookEx(IntPtr hhk, int nCode, IntPtr wParam, IntPtr lParam);
    [DllImport("kernel32.dll")] public static extern IntPtr GetModuleHandleW(string name);
    [DllImport("user32.dll")] public static extern int GetMessage(out MSG msg, IntPtr hWnd, uint min, uint max);
    [DllImport("user32.dll")] public static extern bool TranslateMessage(ref MSG msg);
    [DllImport("user32.dll")] public static extern IntPtr DispatchMessage(ref MSG msg);
    [DllImport("user32.dll")] public static extern bool PostThreadMessage(uint tid, uint msg, IntPtr w, IntPtr l);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [StructLayout(LayoutKind.Sequential)]
    public struct MSG { public IntPtr hwnd; public uint message; public IntPtr wParam; public IntPtr lParam; public uint time; public int ptX; public int ptY; }

    public const int WH_KEYBOARD_LL = 13;
    public static int KeyEventCount = 0;
    public static int KeyDownCount = 0;
    public static int InjectedCount = 0;    // 合成（SendInput 注入）事件数：用来"自校准"钩子是否真的在工作
    public static int PhysicalCount = 0;    // 物理事件数（未带 LLKHF_INJECTED 标记）

    [StructLayout(LayoutKind.Sequential)]
    public struct KBDLLHOOKSTRUCT { public uint vkCode; public uint scanCode; public uint flags; public uint time; public IntPtr dwExtraInfo; }
    public const uint LLKHF_INJECTED = 0x00000010;
    static HookProc _proc;   // 必须保持引用，否则委托被回收、钩子失效

    public static bool InstallKeyboardCounter()
    {
        _proc = new HookProc(Callback);
        IntPtr h = SetWindowsHookExW(WH_KEYBOARD_LL, _proc, GetModuleHandleW(null), 0);
        if (h == IntPtr.Zero) return false;
        _hook = h;
        return true;
    }
    static IntPtr _hook = IntPtr.Zero;
    static IntPtr Callback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0)
        {
            KeyEventCount++;
            if (wParam == (IntPtr)0x0100 || wParam == (IntPtr)0x0104) KeyDownCount++;  // WM_KEYDOWN / WM_SYSKEYDOWN
            KBDLLHOOKSTRUCT kb = (KBDLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(KBDLLHOOKSTRUCT));
            if ((kb.flags & LLKHF_INJECTED) != 0) InjectedCount++; else PhysicalCount++;
        }
        return CallNextHookEx(_hook, nCode, wParam, lParam);
    }

    // 泵消息 HoldSeconds 秒（低层钩子必须有消息循环才会被回调）
    [DllImport("user32.dll")] public static extern bool PeekMessage(out MSG msg, IntPtr hWnd, uint min, uint max, uint remove);
    public const uint PM_REMOVE = 0x0001;

    // 注意：**不能用阻塞式 GetMessage**（没有消息时会永久阻塞，脚本会卡死——5.1 实测踩过）。
    // 低层钩子的回调只要有消息泵就会被调用，所以用 PeekMessage 轮询 + Sleep 即可。
    public static void PumpFor(int seconds)
    {
        DateTime end = DateTime.Now.AddSeconds(seconds);
        while (DateTime.Now < end)
        {
            MSG msg;
            while (PeekMessage(out msg, IntPtr.Zero, 0, 0, PM_REMOVE))
            {
                TranslateMessage(ref msg);
                DispatchMessage(ref msg);
            }
            System.Threading.Thread.Sleep(10);
        }
    }

    public static bool UninstallHook()
    {
        if (_hook == IntPtr.Zero) return true;
        return UnhookWindowsHookEx(_hook);
    }
}
'@
}
$P = [YzInputProbe]

# ---------------------------------------------------------------- 目标窗口
$targetHwnd = [IntPtr]::Zero
if ($TargetProc) {
    $p = Get-Process -Name ($TargetProc -replace '\.exe$', '') -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($p) {
        $p.Refresh()
        if ($p.MainWindowHandle -ne 0) { $targetHwnd = [IntPtr]$p.MainWindowHandle }
        else {
            # 该进程没有主窗口（例如抓屏子进程 / 后台进程），退而找它的任意可见窗口
            Say ("{0} 没有主窗口（pid={1}），B 试验将改用前台窗口" -f $TargetProc, $p.Id)
        }
    } else {
        Say ("找不到进程 {0}，B 试验将改用前台窗口" -f $TargetProc)
    }
}
if ($targetHwnd -eq [IntPtr]::Zero) { $targetHwnd = $P::GetForegroundWindow() }
Say ("B 试验的目标窗口: " + $P::DescribeWindow($targetHwnd))
Say ''

# ---------------------------------------------------------------- C：先装低层钩子
Say ("---- C. 低层键盘钩子计数（{0} 秒）----" -f $HoldSeconds)
$hookOk = $P::InstallKeyboardCounter()
Say ("  SetWindowsHookEx(WH_KEYBOARD_LL) 结果: " + $hookOk)
$cnt = 0
if ($hookOk) {
    # ---- 自校准：脚本自己发一次合成按键，钩子必须看得到（否则"没收到"不能说明被拦）----
    Say '  [自校准] 脚本先发一次合成按键（SendInput，带 INJECTED 标记）…'
    [void]$P::SendUnicodeText('Q')
    $P::PumpFor(1)
    $injCal = $P::InjectedCount
    Say ("  [自校准] 钩子收到合成事件 {0} 次 ⇒ 钩子{1}" -f $injCal, $(if ($injCal -gt 0) { '工作正常' } else { '**没工作**（下面的结论不可用）' }))

    Say ("  请在这 {0} 秒里**用物理键盘随便敲几个键**…" -f $HoldSeconds)
    $P::PumpFor($HoldSeconds)
    $cnt     = $P::KeyEventCount
    $physCnt = $P::PhysicalCount
    [void]$P::UninstallHook()
    Say ("  低层钩子收到: 总计 {0} 次（合成 {1} / 物理 {2}）" -f $cnt, $P::InjectedCount, $physCnt)
    # 关键的人工确认：没按键时的 0 次**不能**当作"被驱动拦了"——否则会得出错误结论。
    $answerC = Read-Host '  刚才那几秒你**确实用物理键盘按了键**吗？(y=按了 / n=没按)'
    Say ("  人工确认: " + $answerC)
    if ($injCal -eq 0) {
        Say '  判读: 自校准就没通过 ⇒ C 试验无结论（别据此判断驱动拦截）'
    } elseif ($answerC -ne 'y') {
        Say '  判读: 你没有按物理键（或没确认）⇒ C 试验**无结论**，不要据此判断驱动拦截。'
    } elseif ($physCnt -eq 0) {
        Say '  判读: 你按了物理键、钩子也确认工作正常，却**一个物理按键都没收到** ⇒ 拦截点在会话之前的设备/驱动层。'
    } else {
        Say '  判读: 物理按键**仍然会派发**到会话 ⇒ 拦截点在用户态（钩子/消息层），我们的拔钩路径有机会解。'
    }
} else {
    Say ("  装钩失败（err={0}）——可能是权限或会话隔离问题，C 无结论" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
}
Say ''

# ---------------------------------------------------------------- A：SendInput
Say '---- A. SendInput（标准输入队列）----'
Say '  请先把光标点进**记事本**（或任意可输入文本的地方），10 秒内会向它发送字符串 YZPROBE 与一次鼠标微移。'
for ($i = 10; $i -gt 0; $i--) { Write-Host ("   {0}…" -f $i) -NoNewline; Write-Host ''; Start-Sleep -Seconds 1 }
$sentText = $P::SendUnicodeText('YZPROBE')
$sentMouse = $P::SendMouseMove(20, 0)
Say ("  SendInput(文字) 返回 {0}（返回的事件数，0 表示被拒）；SendInput(鼠标) 返回 {1}" -f $sentText, $sentMouse)
$answerA = Read-Host '  记事本里出现了 YZPROBE 吗？(y=出现 / n=没出现)'
Say ("  人工观察: " + $answerA)
Say ''

# ---------------------------------------------------------------- B：PostMessage
Say '---- B. PostMessage（绕过输入队列，直接投消息给目标窗口）----'
$sentB = 0
foreach ($ch in 'YZPROBE'.ToCharArray()) {
    $vk = [int][char]::ToUpper($ch)
    if ($P::PostMessage($targetHwnd, $P::WM_KEYDOWN, [IntPtr]$vk, [IntPtr]0)) { $sentB++ }
    if ($P::PostMessage($targetHwnd, $P::WM_CHAR, [IntPtr][int]$ch, [IntPtr]0)) { $sentB++ }
    if ($P::PostMessage($targetHwnd, $P::WM_KEYUP, [IntPtr]$vk, [IntPtr]0)) { $sentB++ }
    Start-Sleep -Milliseconds 60
}
Say ("  已向目标窗口投递 {0} 条按键消息" -f $sentB)
$answerB = Read-Host '  目标窗口有任何反应吗（出现文字/被触发）？(y=有 / n=没有 / s=目标不是文本窗，无法判断)'
Say ("  人工观察: " + $answerB)
Say ''

# ---------------------------------------------------------------- 结论
Say '==== 结论（现场判读用）===='
Say ("  A SendInput: " + $answerA + "    B PostMessage: " + $answerB)
if ($hookOk) { Say ("  C 低层钩子事件数: 总计 {0} / 合成 {1} / 物理 {2}" -f $P::KeyEventCount, $P::InjectedCount, $P::PhysicalCount) }
Say '  组合判读：'
Say '    A=y                      ⇒ 输入队列没被拦，问题只在目标程序的用户态钩子 → 用拔钩/消息层解'
Say '    A=n, B=y                 ⇒ 队列被拦（用户态钩子或驱动），但目标程序认消息 → 用消息层解'
Say '    A=n, C>0                 ⇒ 物理按键还在派发 ⇒ 拦截在用户态队列过滤层，值得继续查'
Say '    A=n, C=0                 ⇒ **设备/驱动级拦截** ⇒ 用户态无解，如实记录并回报，不越界'
Say ''
$log | Set-Content -LiteralPath $outFile -Encoding UTF8
Write-Host ''
Write-Host ("结果已写入: " + $outFile) -ForegroundColor Green
