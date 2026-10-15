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
    [string]$OutDir = '',
[switch]$NoUi            # 自动化测试用：不弹窗，退回控制台提问
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrEmpty($OutDir)) { $OutDir = (Get-Location).Path }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outFile = Join-Path $OutDir ("input-probe-{0}.txt" -f $stamp)

$log = New-Object System.Collections.Generic.List[string]
function Say([string]$s) {
    $log.Add($s)
    Write-Host $s
    try { Add-Content -LiteralPath $outFile -Value $s -Encoding UTF8 -ErrorAction Stop } catch { }
}

# 提问一律走弹窗（是/否/取消），不再用 Read-Host 读控制台。
# 原因（2026-10-15 现场）：C 节要求"用物理键盘敲几个键"，那些按键会落进同一个控制台缓冲区，
# 随后 Read-Host 收到的就是 Q55555555fssdfsdfsdfy / Qy 这种串 ⇒ 两次都被判"无结论"。
# 弹窗不受键盘缓冲影响。带 -NoUi 时退回控制台（自动化测试用），并按"含 y / 含 s"容错解析。
function Ask([string]$prompt, [string]$map = '是=y / 否=n') {
    if ($NoUi) {
        $a = [string](Read-Host ('  ' + $prompt + '  (' + $map + ')'))
        if ($a -match '[yY]') { return 'y' }
        if ($a -match '[sS]') { return 's' }
        return 'n'
    }
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction SilentlyContinue
    $r = [System.Windows.Forms.MessageBox]::Show(($prompt + "`r`n`r`n" + $map), 'YZTrainer 输入探针',
                                                 [System.Windows.Forms.MessageBoxButtons]::YesNoCancel,
                                                 [System.Windows.Forms.MessageBoxIcon]::Question)
    if ($r -eq [System.Windows.Forms.DialogResult]::Yes) { return 'y' }
    if ($r -eq [System.Windows.Forms.DialogResult]::No)  { return 'n' }
    if ($map -match '取消=') { return 's' }
    return 'n'
}

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

    // ---- 低层鼠标钩子：键盘与鼠标可能被**不同的**过滤驱动处理（2026-10-15 现场实测：键盘类
    //      UpperFilters 里多了一个第三方的 kafe，鼠标类只有微软的 mouclass），所以两者要分开数。
    public const int WH_MOUSE_LL = 14;
    public static int MouseEventCount = 0;
    public static int MouseInjectedCount = 0;
    public static int MousePhysicalCount = 0;
    [StructLayout(LayoutKind.Sequential)]
    public struct MSLLHOOKSTRUCT { public int ptX; public int ptY; public uint mouseData; public uint flags; public uint time; public IntPtr dwExtraInfo; }
    public const uint LLMHF_INJECTED = 0x00000001;
    static HookProc _mproc;   // 同样必须保持引用
    static IntPtr _mhook = IntPtr.Zero;
    public static bool InstallMouseCounter()
    {
        _mproc = new HookProc(MouseCallback);
        IntPtr h = SetWindowsHookExW(WH_MOUSE_LL, _mproc, GetModuleHandleW(null), 0);
        if (h == IntPtr.Zero) return false;
        _mhook = h;
        return true;
    }
    static IntPtr MouseCallback(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0)
        {
            MouseEventCount++;
            MSLLHOOKSTRUCT ms = (MSLLHOOKSTRUCT)Marshal.PtrToStructure(lParam, typeof(MSLLHOOKSTRUCT));
            if ((ms.flags & LLMHF_INJECTED) != 0) MouseInjectedCount++; else MousePhysicalCount++;
        }
        return CallNextHookEx(_mhook, nCode, wParam, lParam);
    }
    public static bool UninstallMouseHook()
    {
        if (_mhook == IntPtr.Zero) return true;
        bool ok = UnhookWindowsHookEx(_mhook);
        _mhook = IntPtr.Zero;
        return ok;
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


# ---------------------------------------------------------------- 目标窗口
$targetHwnd = [IntPtr]::Zero
if ($TargetProc) {
    $tpObj = Get-Process -Name ($TargetProc -replace '\.exe$', '') -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($tpObj) {
        $tpObj.Refresh()
        if ($tpObj.MainWindowHandle -ne 0) { $targetHwnd = [IntPtr]$tpObj.MainWindowHandle }
        else {
            # 该进程没有主窗口（例如抓屏子进程 / 后台进程），退而找它的任意可见窗口
            Say ("{0} 没有主窗口（pid={1}），B 试验将改用前台窗口" -f $TargetProc, $tpObj.Id)
        }
    } else {
        Say ("找不到进程 {0}，B 试验将改用前台窗口" -f $TargetProc)
    }
}
if ($targetHwnd -eq [IntPtr]::Zero) { $targetHwnd = [YzInputProbe]::GetForegroundWindow() }
Say ("B 试验的目标窗口: " + [YzInputProbe]::DescribeWindow($targetHwnd))
Say ''

# ---------------------------------------------------------------- C：先装低层钩子
Say ("---- C. 低层键盘钩子计数（{0} 秒）----" -f $HoldSeconds)
# 先记录"测试那一刻的封锁状态"——否则事后无法判断这些数字是"正常输入"还是"被拦时的输入"
# （2026-10-15 下午那趟没记，数据因此无法定论）。
$kbBlocked = Ask '开始测试前：现在**键盘**被远志屏蔽了吗？' '是=正在被屏蔽（按不动）/ 否=正常'
Say ("  测试时的封锁状态：键盘=" + $(if ($kbBlocked -eq 'y') { '被屏蔽' } else { '正常' }))
$msBlocked = Ask '（同一时刻）现在**鼠标**被远志屏蔽了吗？' '是=正在被屏蔽（点不动 / 光标被拽走）/ 否=正常'
Say ("  测试时的封锁状态：鼠标=" + $(if ($msBlocked -eq 'y') { '被屏蔽' } else { '正常' }))
$hookOk = [YzInputProbe]::InstallKeyboardCounter()
Say ("  SetWindowsHookEx(WH_KEYBOARD_LL) 结果: " + $hookOk)
$mhookOk = [YzInputProbe]::InstallMouseCounter()
Say ("  SetWindowsHookEx(WH_MOUSE_LL) 结果: " + $mhookOk)
$cnt = 0
if ($hookOk) {
    # ---- 自校准：脚本自己发一次合成按键，钩子必须看得到（否则"没收到"不能说明被拦）----
    Say '  [自校准] 脚本先发一次合成按键（SendInput，带 INJECTED 标记）…'
    [void][YzInputProbe]::SendUnicodeText('Q')
    [void][YzInputProbe]::SendMouseMove(5, 5)
    [YzInputProbe]::PumpFor(1)
    $injCal = [YzInputProbe]::InjectedCount
    $injCalM = [YzInputProbe]::MouseInjectedCount
    Say ("  [自校准] 鼠标钩子收到合成事件 {0} 次 ⇒ 鼠标钩子{1}" -f $injCalM, $(if ($injCalM -gt 0) { '工作正常' } else { '**没工作**（鼠标结论不可用）' }))
    Say ("  [自校准] 钩子收到合成事件 {0} 次 ⇒ 钩子{1}" -f $injCal, $(if ($injCal -gt 0) { '工作正常' } else { '**没工作**（下面的结论不可用）' }))

    Say ("  请在这 {0} 秒里做两件事：① **用物理键盘随便敲几个键**；② **动一动鼠标并点一两下**…" -f $HoldSeconds)
    [YzInputProbe]::PumpFor($HoldSeconds)
    $cnt     = [YzInputProbe]::KeyEventCount
    $physCnt = [YzInputProbe]::PhysicalCount
    $mcnt     = [YzInputProbe]::MouseEventCount
    $mphysCnt = [YzInputProbe]::MousePhysicalCount
    [void][YzInputProbe]::UninstallHook()
    [void][YzInputProbe]::UninstallMouseHook()
    Say ("  键盘低层钩子收到: 总计 {0} 次（合成 {1} / 物理 {2}）" -f $cnt, [YzInputProbe]::InjectedCount, $physCnt)
    Say ("  鼠标低层钩子收到: 总计 {0} 次（合成 {1} / 物理 {2}）" -f $mcnt, [YzInputProbe]::MouseInjectedCount, $mphysCnt)
    # 关键的人工确认：没按键时的 0 次**不能**当作"被驱动拦了"——否则会得出错误结论。
    $answerC = Ask '刚才那几秒你确实用物理键盘按了键吗？' '是=按了 / 否=没按'
    Say ("  人工确认（键盘）: " + $answerC)
    $answerM = Ask '刚才那几秒你确实动过鼠标（移动或点击）吗？' '是=动过 / 否=没动'
    Say ("  人工确认（鼠标）: " + $answerM)
    if ($injCal -eq 0) {
        Say '  判读: 键盘自校准没通过 ⇒ 键盘部分无结论（别据此判断驱动拦截）'
    } else {
        if ($answerC -eq 'y') {
            if ($physCnt -eq 0) { Say '  键盘判读: 按了物理键、钩子也正常，却**一个物理按键都没收到** ⇒ 拦截在会话之前的设备/驱动层。' }
            else                { Say ('  键盘判读: 物理按键**仍然会派发**到会话（{0} 次）⇒ 拦截在用户态（钩子/消息层），我们的拔钩路径有机会解。' -f $physCnt) }
        } else { Say '  键盘判读: 没确认按过物理键 ⇒ 键盘这一项无结论。' }
        if ($injCalM -eq 0) { Say '  鼠标判读: 鼠标自校准没通过 ⇒ 鼠标部分无结论。' }
        elseif ($answerM -eq 'y') {
            if ($mphysCnt -eq 0) { Say '  鼠标判读: 动了鼠标、钩子也正常，却**一个物理鼠标事件都没收到** ⇒ 拦截在会话之前的设备/驱动层。' }
            else                 { Say ('  鼠标判读: 物理鼠标事件**仍然会派发**到会话（{0} 次）⇒ 拦截在用户态（钩子/消息层）。' -f $mphysCnt) }
        } else { Say '  鼠标判读: 没确认动过鼠标 ⇒ 鼠标这一项无结论。' }
        Say '  （键盘与鼠标必须分开看：2026-10-15 现场键盘类 UpperFilters 里有第三方 kafe，鼠标类只有微软的 mouclass。）'
    }
} else {
    Say ("  装钩失败（err={0}）——可能是权限或会话隔离问题，C 无结论" -f [Runtime.InteropServices.Marshal]::GetLastWin32Error())
}
Say ''

# ---------------------------------------------------------------- A：SendInput
Say '---- A. SendInput（标准输入队列）----'
Say '  请先把光标点进**记事本**（或任意可输入文本的地方），10 秒内会向它发送字符串 YZPROBE 与一次鼠标微移。'
for ($i = 10; $i -gt 0; $i--) { Write-Host ("   {0}…" -f $i) -NoNewline; Write-Host ''; Start-Sleep -Seconds 1 }
$sentText = [YzInputProbe]::SendUnicodeText('YZPROBE')
$sentMouse = [YzInputProbe]::SendMouseMove(20, 0)
Say ("  SendInput(文字) 返回 {0}（返回的事件数，0 表示被拒）；SendInput(鼠标) 返回 {1}" -f $sentText, $sentMouse)
$answerA = Ask '记事本里出现了 YZPROBE 吗？' '是=出现 / 否=没出现'
Say ("  人工观察: " + $answerA)
Say ''

# ---------------------------------------------------------------- B：PostMessage
Say '---- B. PostMessage（绕过输入队列，直接投消息给目标窗口）----'
$sentB = 0
foreach ($ch in 'YZPROBE'.ToCharArray()) {
    $vk = [int][char]::ToUpper($ch)
    if ([YzInputProbe]::PostMessage($targetHwnd, [YzInputProbe]::WM_KEYDOWN, [IntPtr]$vk, [IntPtr]0)) { $sentB++ }
    if ([YzInputProbe]::PostMessage($targetHwnd, [YzInputProbe]::WM_CHAR, [IntPtr][int]$ch, [IntPtr]0)) { $sentB++ }
    if ([YzInputProbe]::PostMessage($targetHwnd, [YzInputProbe]::WM_KEYUP, [IntPtr]$vk, [IntPtr]0)) { $sentB++ }
    Start-Sleep -Milliseconds 60
}
Say ("  已向目标窗口投递 {0} 条按键消息" -f $sentB)
$answerB = Ask '目标窗口有任何反应吗（出现文字/被触发）？' '是=有反应 / 否=没反应 / 取消=目标不是文本窗，无法判断'
Say ("  人工观察: " + $answerB)
Say ''

# ---------------------------------------------------------------- 结论
Say '==== 结论（现场判读用）===='
Say ("  A SendInput: {0}    B PostMessage: {1}" -f $answerA, $answerB)
Say ("  （A 发出的字符串=YZPROBE；SendInput 返回事件数={0}，鼠标微移返回={1}）" -f $sentText, $sentMouse)
if ($hookOk)  { Say ("  C 键盘低层钩子: 总计 {0} / 合成 {1} / 物理 {2}" -f [YzInputProbe]::KeyEventCount, [YzInputProbe]::InjectedCount, [YzInputProbe]::PhysicalCount) }
if ($mhookOk) { Say ("  C 鼠标低层钩子: 总计 {0} / 合成 {1} / 物理 {2}" -f [YzInputProbe]::MouseEventCount, [YzInputProbe]::MouseInjectedCount, [YzInputProbe]::MousePhysicalCount) }
Say ("  测试时的封锁状态（用户确认）: 键盘={0}  鼠标={1}" -f $(if ($kbBlocked -eq 'y') { '被屏蔽' } else { '正常' }), $(if ($msBlocked -eq 'y') { '被屏蔽' } else { '正常' }))
Say '  组合判读：'
Say '    A=y                      ⇒ 输入队列没被拦，问题只在目标程序的用户态钩子 → 用拔钩/消息层解'
Say '    A=n, B=y                 ⇒ 队列被拦（用户态钩子或驱动），但目标程序认消息 → 用消息层解'
Say '    A=n, C(物理)>0           ⇒ 物理按键还在派发 ⇒ 拦截在用户态队列过滤层，值得继续查'
Say '    A=n, C(物理)=0 且自校准通过且你确认按过 ⇒ **设备/驱动级拦截** ⇒ 用户态无解，如实记录并回报，不越界'
Say ''
# ---------------------------------------------------------------- D：驱动栈侦察（只读）
# 目的：不碰任何驱动，只把"谁在这条链路里"读出来。明天在机房若 A/B/C 指向设备级拦截，
#       这份清单就是唯一的依据（哪个厂商过滤驱动、挂在哪个类上）。
Say ''
Say '---- D. 驱动栈侦察（只读：注册表类过滤 + 运行中的相关内核驱动）----'
$classGuids = @{
    '键盘类 {4d36e96b-e325-11ce-bfc1-08002be10318}' = '{4d36e96b-e325-11ce-bfc1-08002be10318}'
    '鼠标类 {4d36e96f-e325-11ce-bfc1-08002be10318}' = '{4d36e96f-e325-11ce-bfc1-08002be10318}'
    'HID 类 {745a17a0-74d3-11d0-b6fe-00a0c90f57da}'  = '{745a17a0-74d3-11d0-b6fe-00a0c90f57da}'
}
foreach ($k in $classGuids.Keys) {
    $base = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\' + $classGuids[$k]
    try {
        $up = (Get-ItemProperty -LiteralPath $base -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
        $lo = (Get-ItemProperty -LiteralPath $base -Name LowerFilters -ErrorAction SilentlyContinue).LowerFilters
        Say ("  {0}: UpperFilters=[{1}]  LowerFilters=[{2}]" -f $k,
             $(if ($up) { ($up -join ', ') } else { '' }),
             $(if ($lo) { ($lo -join ', ') } else { '' }))
    } catch {
        Say ("  {0}: 读取失败（{1}）" -f $k, $_.Exception.Message)
    }
}
# 把上面列到的每个过滤驱动解析出来：服务 → 二进制路径 → 版本/签名。
# 2026-10-15 现场在键盘类上读到 UpperFilters=[kbdclass, kafe]，其中 kafe 不是微软自带的名字，
# 这种线索必须能落到"哪个文件、哪个厂商、还装不装"，否则现场只能凭名字猜。
$filterNames = @()
foreach ($k in $classGuids.Keys) {
    $base = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\' + $classGuids[$k]
    $up = (Get-ItemProperty -LiteralPath $base -Name UpperFilters -ErrorAction SilentlyContinue).UpperFilters
    $lo = (Get-ItemProperty -LiteralPath $base -Name LowerFilters -ErrorAction SilentlyContinue).LowerFilters
    foreach ($n in @($up) + @($lo)) { if ($n) { $filterNames += $n } }
}
$filterNames = @($filterNames | Sort-Object -Unique)
if ($filterNames.Count -gt 0) {
    $startMap = @{ 0 = 'Boot'; 1 = 'System'; 2 = 'Auto'; 3 = 'Manual'; 4 = 'Disabled' }
    Say '  这些过滤驱动的解析（只读，逐个查服务与文件）：'
    foreach ($n in $filterNames) {
        $svc = Get-ItemProperty -LiteralPath ('HKLM:\SYSTEM\CurrentControlSet\Services\' + $n) -ErrorAction SilentlyContinue
        if (-not $svc) { Say ("    {0}: 注册表里没有同名服务（可能是内置类驱动）" -f $n); continue }
        $img  = [string]$svc.ImagePath
        $path = $img -replace '^\\\?\?\\', '' -replace '^\\SystemRoot\\', ($env:SystemRoot + '\') -replace '^system32\\', ($env:SystemRoot + '\system32\')
        $st   = if ($startMap.ContainsKey([int]$svc.Start)) { $startMap[[int]$svc.Start] } else { [string]$svc.Start }
        Say ("    {0}: Start={1} Type={2} 文件={3}" -f $n, $st, $svc.Type, $(if ($path) { $path } else { '(空)' }))
        if ($path -and (Test-Path -LiteralPath $path -ErrorAction SilentlyContinue)) {
            $fi = Get-Item -LiteralPath $path -ErrorAction SilentlyContinue
            if ($fi) { Say ("        版本={0}" -f $fi.VersionInfo.FileVersion) }
            try {
                if (Get-Command Get-AuthenticodeSignature -ErrorAction SilentlyContinue) {
                    $sg = Get-AuthenticodeSignature -LiteralPath $path -ErrorAction SilentlyContinue
                    if ($sg -and $sg.SignerCertificate) { Say ("        签名主体={0}" -f $sg.SignerCertificate.Subject) }
                } else {
                    Say '        （本机读不了 Authenticode 签名：Security 模块不可用，不影响其它判据）'
                }
            } catch { Say '        （读签名时出错，跳过）' }
        }
    }
}
# 非微软厂商的运行中内核驱动（用文件版本信息里的 CompanyName 判定，快且不需要签名模块）。
# 2026-10-15 现场在键盘类上读到第三方的 kafe，但按名字模式扫运行驱动时它一条都没出现——
# 这条能把"厂商装的、正在跑的"驱动直接列出来。
Say '  非微软厂商的运行中内核驱动（CompanyName 里不含 Microsoft 的）：'
try {
    $nonMs = @()
    foreach ($d in (Get-CimInstance Win32_SystemDriver -ErrorAction Stop | Where-Object { $_.State -eq 'Running' -and $_.PathName })) {
        $dp = [string]$d.PathName -replace '^\\\?\?\\', '' -replace '^\\SystemRoot\\', ($env:SystemRoot + '\') -replace '^system32\\', ($env:SystemRoot + '\system32\')
        if (-not (Test-Path -LiteralPath $dp -ErrorAction SilentlyContinue)) { continue }
        $co = ''
        try { $co = (Get-Item -LiteralPath $dp -ErrorAction SilentlyContinue).VersionInfo.CompanyName } catch { }
        if ($co -and ($co -notmatch 'Microsoft')) { $nonMs += ("{0}（{1}）" -f $d.Name, $co) }
    }
    if ($nonMs.Count -eq 0) { Say '    （没有：运行中的驱动全部来自微软）' }
    else { foreach ($x in ($nonMs | Sort-Object -Unique)) { Say ('    ' + $x) } }
} catch { Say ('    读取失败（{0}）' -f $_.Exception.Message) }
Say '  运行中的相关内核驱动（名字或路径命中 kbd/mou/hid/filter/hook/yz/exd/ks/teach/class）：'
try {
    $drv = Get-CimInstance Win32_SystemDriver -ErrorAction Stop | Where-Object {
        $_.State -eq 'Running' -and ($_.Name + ' ' + $_.PathName) -match '(?i)kbd|mou|hid|filter|hook|yz|exd|ks|teach|class'
    }
    if ($drv) {
        foreach ($x in ($drv | Sort-Object Name)) {
            Say ("    {0,-22} {1,-10} {2}" -f $x.Name, $x.StartMode, $x.PathName)
        }
    } else {
        Say '    （没有命中项——可能确实干净，也可能是权限不足看不到）'
    }
} catch {
    Say ("    枚举失败（{0}）" -f $_.Exception.Message)
}
Say '  说明：本工具自身**不安装任何驱动**；上面这些若出现厂商名字，那才是"设备级拦截"的候选。'
Say ''
Write-Host ''
Write-Host ("结果已写入: " + $outFile) -ForegroundColor Green

