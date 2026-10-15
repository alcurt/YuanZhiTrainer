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
[switch]$NoUi,           # 自动化测试用：不弹窗，退回控制台提问
[int]$WatchMinutes = 0   # >0 ⇒ 待锁观察模式：被动采样这么久（锁屏期间不需要任何操作）
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
    // ---- 交叉判据（v0.6.42 补）：不经过用户态钩子链的第二条通路 ----
    // 为什么要它：远志在自己进程里装的 WH_KEYBOARD_LL 只要不调 CallNextHookEx 就能把事件吞掉，
    // 我们**同样收不到** ⇒ 单看"低层钩子 0 次"无法区分"设备层拦截"和"用户态钩子吞掉了"。
    // 而按键状态（GetAsyncKeyState）与光标位置（GetCursorPos）来自操作系统的另一条路：
    //   钩子有、状态有  ⇒ 事件确实到了会话（用户态可解）
    //   钩子无、状态有  ⇒ 消息路径被用户态钩子吞了（同样用户态可解）
    //   钩子无、状态无  ⇒ 事件根本没到操作系统（设备/驱动层）
    [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vKey);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT pt);
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X; public int Y; }
    public static string DownKeys()   // 当前处于按下状态的虚拟键（含鼠标左右键 0x01/0x02），十六进制逗号分隔
    {
        StringBuilder sb = new StringBuilder();
        for (int vk = 1; vk < 256; vk++)
        {
            if ((GetAsyncKeyState(vk) & 0x8000) != 0)
            {
                if (sb.Length > 0) sb.Append(',');
                sb.Append(vk.ToString("X2"));
            }
        }
        return sb.ToString();
    }
    public static int CursorX = -1, CursorY = -1;
    public static void SampleCursor() { POINT p; if (GetCursorPos(out p)) { CursorX = p.X; CursorY = p.Y; } }
    public static bool PumpOnce()     // 非阻塞泵一次（待锁模式要长时间轮询，不能用 PumpFor 卡住）
    {
        bool any = false; MSG msg;
        while (PeekMessage(out msg, IntPtr.Zero, 0, 0, PM_REMOVE)) { TranslateMessage(ref msg); DispatchMessage(ref msg); any = true; }
        return any;
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

# ---------------------------------------------------------------- W：待锁观察（可选，提前分支）
# 为什么要有这个模式（2026-10-15 用户指出）：键鼠被锁住的那一刻，人**没法点弹窗、没法敲命令**，
# 所以"在被锁的瞬间手动跑探针"这个要求本身是自相矛盾的。正确做法是上课前先挂上，让它被动等锁：
# 锁屏期间人只要随手按几下物理键、动几下鼠标（物理动作不受锁影响），其余全靠它自己采样。
if ($WatchMinutes -gt 0) {
    Say ''
    Say ("---- W. 待锁观察模式（最长 {0} 分钟；**被锁期间你不需要操作这个界面**）----" -f $WatchMinutes)
    Say '  现在别关这个窗口。老师一开始锁屏，你就随手：① 敲几下物理键；② 动几下鼠标、点两下。'
    Say '  到时间（或你按 Ctrl+C）它会把这段时间的判据打成一张表，并问你两个问题。'
    $whookKb = [YzInputProbe]::InstallKeyboardCounter()
    $whookMs = [YzInputProbe]::InstallMouseCounter()
    Say ("  钩子就绪: 键盘={0} 鼠标={1}" -f $whookKb, $whookMs)
    $wEnd = (Get-Date).AddMinutes($WatchMinutes)
    $wKb = 0; $wMs = 0; $wLastKeys = ''; $wEvents = @(); $wSamples = 0; $wZero = 0
    $wAsyncNoHook = 0; $wKeyChanges = 0; $wRefresh = 0; $wHeartbeat = (Get-Date).AddSeconds(30)
    # 光标轨迹统计（用户 2026-10-15 描述的现场现象：被锁时鼠标能移动，但几百毫秒就被拽回左上角）
    $wAwayStart = $null; $wAwaySpans = @(); $wAwayMax = 0; $wAwayMaxDist = 0
    try {
        while ((Get-Date) -lt $wEnd) {
            [void][YzInputProbe]::PumpOnce()
            $down = [YzInputProbe]::DownKeys()
            [YzInputProbe]::SampleCursor()
            $wSamples++
            $cx = [YzInputProbe]::CursorX; $cy = [YzInputProbe]::CursorY
            if ($cx -eq 0 -and $cy -eq 0) {
                $wZero++
                if ($null -ne $wAwayStart) {          # 一段"离开左上角"到此结束 ⇒ 记下它有多久
                    $dur = ((Get-Date) - $wAwayStart).TotalMilliseconds
                    $wAwaySpans += [int]$dur
                    if ($dur -gt $wAwayMax) { $wAwayMax = $dur }
                    $wAwayStart = $null
                }
            } else {
                if ($null -eq $wAwayStart) { $wAwayStart = Get-Date }
                $dist = [Math]::Sqrt(($cx * $cx) + ($cy * $cy))
                if ($dist -gt $wAwayMaxDist) { $wAwayMaxDist = $dist }
            }
            $kb = [YzInputProbe]::KeyEventCount; $ms = [YzInputProbe]::MouseEventCount
            if ($down -ne $wLastKeys -and $down -ne '') {
                $wKeyChanges++
                $llNew = ($kb - $wKb) + ($ms - $wMs)
                if ($llNew -eq 0) { $wAsyncNoHook++ }
                $wEvents += ("  t={0} 按键状态变为 [{1}]，期间低层钩子新增事件 {2}，光标={3},{4}" -f (Get-Date -Format 'HH:mm:ss'), $down, $llNew, [YzInputProbe]::CursorX, [YzInputProbe]::CursorY)
            }
            if ($kb -ne $wKb -or $ms -ne $wMs) {
                $wEvents += ("  t={0} 低层钩子新增：键盘 {1}→{2} 鼠标 {3}→{4}（keys=[{5}] 光标={6},{7}）" -f (Get-Date -Format 'HH:mm:ss'), $wKb, $kb, $wMs, $ms, $down, [YzInputProbe]::CursorX, [YzInputProbe]::CursorY)
            }
            $wKb = $kb; $wMs = $ms; $wLastKeys = $down
            if ((Get-Date) -gt $wHeartbeat) {
                $wHeartbeat = (Get-Date).AddSeconds(30)
                Say ("  [心跳 {0}] 采样 {1} 次；按键变化 {2} 次（其中钩子全无 {3} 次）；钩子累计 键盘 {4} / 鼠标 {5}；光标停(0,0) {6} 次" -f (Get-Date -Format 'HH:mm:ss'), $wSamples, $wKeyChanges, $wAsyncNoHook, $wKb, $wMs, $wZero)
            }
            # 每秒重装一次自己的低层钩子：本机实测"**后装的钩子先被调用**"（顺序是 B 先 A 后），
            # 所以重装能让我们始终站在钩子链头 ⇒ 远志即使在自己进程里吞事件，我们也先看到它。
            # 这是本机唯一验证过能穿透"用户态钩子吞事件"的观测法（GetAsyncKeyState 与 Raw Input
            # 都在钩子链下游，实测都会被一起吞掉）。
            if ((($wSamples) % 20) -eq 0) {
                [void][YzInputProbe]::UninstallHook()
                [void][YzInputProbe]::UninstallMouseHook()
                [void][YzInputProbe]::InstallKeyboardCounter()
                [void][YzInputProbe]::InstallMouseCounter()
                $wRefresh++
            }
            Start-Sleep -Milliseconds 50
        }
    } finally {
        [void][YzInputProbe]::UninstallHook()
        [void][YzInputProbe]::UninstallMouseHook()
        # 收尾：采样结束时若光标仍在外面，把这一段也算上（否则"停(0,0)=0 次"却报"没离开过"这种自相矛盾）
        if ($null -ne $wAwayStart) {
            $dur = ((Get-Date) - $wAwayStart).TotalMilliseconds
            $wAwaySpans += [int]$dur
            if ($dur -gt $wAwayMax) { $wAwayMax = $dur }
            $wAwayStart = $null
        }
    }
    Say ''
    Say '  ---- 待锁观察汇总 ----'
    Say ("  采样 {0} 次（约 {1} 分钟）；按键状态变化 {2} 次" -f $wSamples, $WatchMinutes, $wKeyChanges)
    Say ("  （参考，不作判据：GetAsyncKeyState 与低层钩子对不上 {0} 次——本机实测钩子吞事件时它同样为 0，两者都在钩子链下游）" -f $wAsyncNoHook)
    Say ("  低层钩子累计: 键盘 {0} 次（物理 {1} / 合成 {2}）  鼠标 {3} 次（物理 {4} / 合成 {5}）" -f `
         [YzInputProbe]::KeyEventCount, [YzInputProbe]::PhysicalCount, [YzInputProbe]::InjectedCount, `
         [YzInputProbe]::MouseEventCount, [YzInputProbe]::MousePhysicalCount, [YzInputProbe]::MouseInjectedCount)
    Say ("  光标停在 (0,0) 的比例: {0:P1}" -f $(if ($wSamples -gt 0) { $wZero / $wSamples } else { 0 }))
    if ($wAwaySpans.Count -gt 0) {
        $sorted = @($wAwaySpans | Sort-Object)
        $median = $sorted[[int]($sorted.Count / 2)]
        Say ("  光标「离开左上角」的自由段: {0} 段；最长 {1} ms、中位 {2} ms；最远离开 (0,0) 约 {3:N0} px" -f $wAwaySpans.Count, [int]$wAwayMax, $median, $wAwayMaxDist)
        Say '    （现场现象：被锁时鼠标能动、但很快被拽回左上角——这段时长就是"能动的窗口"；若它只有几百毫秒，' 
        Say '      说明有东西在**周期性**把光标拉回去。结合日志里"[输入取证] SetCursorPos"能判断它是不是学生端进程自己干的。）'
    } elseif ($wZero -eq $wSamples -and $wSamples -gt 0) {
        Say '  光标全程都停在 (0,0)（一次都没离开）——若有东西在周期性把它拽回去，就是这个形态。'
    } else {
        Say '  光标有离开 (0,0) 的采样，但没有形成完整自由段（采样过稀或刚离开就结束）。'
    }
    if ($wEvents.Count -gt 0) {
        Say '  事件时间线（最多 40 条）：'
        $wEvents | Select-Object -First 40 | ForEach-Object { Say ('  ' + $_) }
    } else { Say '  这段时间没有任何按键/鼠标活动（可能老师没锁、你也没按）。' }
    Say ''
    Say '  现在回答两个问题（此时输入已恢复，弹窗能点了）：'
    $wLocked  = Ask '刚才这段时间里，键鼠有没有被远志屏蔽过？' '是=有被锁过 / 否=没有被锁'
    $wPressed = Ask '这段时间里你有没有真的按过键、动过鼠标？' '是=有 / 否=没有'
    Say ("  人工确认: 期间被锁={0}  期间操作过={1}" -f $(if ($wLocked -eq 'y') { '是' } else { '否' }), $(if ($wPressed -eq 'y') { '是' } else { '否' }))
    Say ("  钩子重装 {0} 次（每次重装后我们是链头，远志的钩子吞不掉我们能看到的事件）" -f $wRefresh)
    if ($wLocked -eq 'y' -and $wPressed -eq 'y') {
        if (([YzInputProbe]::KeyEventCount + [YzInputProbe]::MouseEventCount) -gt 0) {
            Say '  判读: 被锁期间事件**照样被我们看到了**（我们始终在钩子链头）⇒ 事件确实到了用户态 ⇒ 拦截在用户态（钩子/消息层），拔钩路径有机会解。'
        } else {
            Say '  判读: 我们全程站在钩子链头、却**一个事件都没看到** ⇒ 事件没进用户态 ⇒ 强指向设备/驱动层拦截（如实记录，不越界；D 节里的非微软驱动是旁证）。'
        }
    } else { Say '  判读: 缺少"确实被锁过 + 确实操作过"这两个前提 ⇒ 这趟不作结论（这正是 2026-10-15 下午那趟的问题）。' }
    Say ''
    Write-Host ("结果已写入: " + $outFile) -ForegroundColor Green
    exit 0
}
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
    # 采样循环（替代一次性 PumpFor）：一边泵消息让低层钩子回调，一边用 GetAsyncKeyState/GetCursorPos
    # 走"不经过用户态钩子链"的那条路做交叉判据 —— 只看钩子计数无法区分"设备层拦截"与"用户态钩子吞事件"。
    $seenKeys = @(); $asyncNoHook = 0; $cursorSamples = 0; $cursorZero = 0
    $kbBefore = [YzInputProbe]::KeyEventCount; $msBefore = [YzInputProbe]::MouseEventCount
    $holdEnd = (Get-Date).AddSeconds($HoldSeconds); $lastDown = ''
    # 采样前重装一次钩子：本机实测"后装的先被调用" ⇒ 重装后我们站在链头，
    # 远志的钩子（即使会吞事件）也吞不掉我们能看到的那一份。
    [void][YzInputProbe]::UninstallHook(); [void][YzInputProbe]::UninstallMouseHook()
    [void][YzInputProbe]::InstallKeyboardCounter(); [void][YzInputProbe]::InstallMouseCounter()
    while ((Get-Date) -lt $holdEnd) {
        [void][YzInputProbe]::PumpOnce()
        $down = [YzInputProbe]::DownKeys()
        [YzInputProbe]::SampleCursor()
        $cursorSamples++
        if ([YzInputProbe]::CursorX -eq 0 -and [YzInputProbe]::CursorY -eq 0) { $cursorZero++ }
        if ($down -ne '' -and $down -ne $lastDown) {
            $seenKeys += $down
            if ((([YzInputProbe]::KeyEventCount - $kbBefore) + ([YzInputProbe]::MouseEventCount - $msBefore)) -eq 0) { $asyncNoHook++ }
        }
        $lastDown = $down
        Start-Sleep -Milliseconds 50
    }
    $cnt     = [YzInputProbe]::KeyEventCount
    $physCnt = [YzInputProbe]::PhysicalCount
    $mcnt     = [YzInputProbe]::MouseEventCount
    $mphysCnt = [YzInputProbe]::MousePhysicalCount
    [void][YzInputProbe]::UninstallHook()
    [void][YzInputProbe]::UninstallMouseHook()
    Say ("  键盘低层钩子收到: 总计 {0} 次（合成 {1} / 物理 {2}）" -f $cnt, [YzInputProbe]::InjectedCount, $physCnt)
    Say ("  鼠标低层钩子收到: 总计 {0} 次（合成 {1} / 物理 {2}）" -f $mcnt, [YzInputProbe]::MouseInjectedCount, $mphysCnt)
    Say ("  参考（不作判据）: GetAsyncKeyState 采到 {0} 次按键状态变化；它与低层钩子对不上 {1} 次" -f $seenKeys.Count, $asyncNoHook)
    Say '    —— 本机实测：钩子把事件吞掉时 GetAsyncKeyState 同样收不到（两者都在钩子链下游），所以判据是"我们已站上链头却什么都没看到"。'
    Say ("  光标采样 {0} 次，其中停在 (0,0) 的 {1} 次" -f $cursorSamples, $cursorZero)
    # 关键的人工确认：没按键时的 0 次**不能**当作"被驱动拦了"——否则会得出错误结论。
    $answerC = Ask '刚才那几秒你确实用物理键盘按了键吗？' '是=按了 / 否=没按'
    Say ("  人工确认（键盘）: " + $answerC)
    $answerM = Ask '刚才那几秒你确实动过鼠标（移动或点击）吗？' '是=动过 / 否=没动'
    Say ("  人工确认（鼠标）: " + $answerM)
    if ($injCal -eq 0) {
        Say '  判读: 键盘自校准没通过 ⇒ 键盘部分无结论（别据此判断驱动拦截）'
    } else {
        if ($answerC -eq 'y') {
            if ($physCnt -gt 0) {
                Say ('  键盘判读: 物理按键**确实到了会话**（低层钩子收到 {0} 次）⇒ 若当时被锁，拦截在用户态，拔钩/消息层有机会解。' -f $physCnt)
            } else {
                Say ('  键盘判读: 采样前我们已重装钩子站上链头，却**一个事件都没看到**（GetAsyncKeyState 另采到 {0} 次状态变化，但本机实测它在钩子吞事件时同样为 0，不作判据）⇒ 强指向设备/驱动层拦截。' -f $seenKeys.Count)
            }
        } else { Say '  键盘判读: 没确认按过物理键 ⇒ 键盘这一项无结论。' }
        if ($injCalM -eq 0) { Say '  鼠标判读: 鼠标自校准没通过 ⇒ 鼠标部分无结论。' }
        elseif ($answerM -eq 'y') {
            if ($mphysCnt -gt 0) {
                Say ('  鼠标判读: 物理鼠标事件**确实到了会话**（{0} 次）⇒ 若当时被锁，拦截在用户态。' -f $mphysCnt)
            } elseif ($cursorSamples -gt 0) {
                Say ('  鼠标判读: 我们已在钩子链头却没收到鼠标事件；光标位置仍可读（采 {0} 次、其中停在 (0,0) 的 {1} 次）——光标被反复拽回 (0,0) 本身就是"有人在管光标"的证据，把它连同 D 节驱动清单一起记下来。' -f $cursorSamples, $cursorZero)
            } else { Say '  鼠标判读: 无可用读数 ⇒ 鼠标这一项无结论。' }
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

