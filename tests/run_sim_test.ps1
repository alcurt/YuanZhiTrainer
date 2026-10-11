<#
    YZTrainer 本地回归测试（不需要机房）：
    用 YZSimTarget 模拟远志学生端的全屏广播、键鼠封锁与定期抢回全屏的行为。

    请以管理员身份运行（YZTrainer.exe 需要提权；自动测试时不希望弹 UAC）。

    阶段一（注入路径，ExternalWindowFix=0，只看进程内 Hook）断言 6 条：
      1. 模拟目标启动后处于全屏置顶状态
      2. YZTrainer 启动后模拟窗口被窗口化（不再是全屏）
      3. 窗口样式已包含 WS_CAPTION（真的变成普通窗口）
      4. 开启防监视后，模拟端连续抓帧哈希保持不变（画面被冻结）
      5. 不受保护的目标走的仍是远程线程注入（v0.6.14：日志出现"已注入目标进程"）
      6. 注入预检没有误判（v0.6.14：日志**不**出现"缺 PROCESS_VM_OPERATION"）
         ——守的是 VmOperationStripped()：它一旦误报，所有机器都会被无谓地降级到消息钩子

    阶段二（免注入兜底，AutoInject=0 + ExternalWindowFix=1）断言 3 条：
      7. 完全不注入的情况下，模拟窗口仍被主程序跨进程改成普通窗口
      8. 外部纠正后的窗口样式包含 WS_CAPTION
      9. 日志里能看到“外部窗口纠正”记录（找不到只记 WARN，不影响窗口化结论）

    阶段三（假全屏，Flags=4|32）断言 2 条：
     10. 广播窗口仍铺满整屏（fullscreen=1）且没有标题栏
     11. 广播窗口已不置顶（topmost=0）——这是"能切到自己的窗口"的前提

    阶段四（远程执行审计，YZSimTarget --exec）断言 2 条：
     12. remote-exec.log 里出现 CreateProcessW 记录
     13. 该记录被标为 origin=remote（不是客户端自己拉起的辅助进程）

    阶段五（主动拔钩的晚加载重试，v0.6.3）断言 1 条：
     14. 钩子模块在注入之后才被加载时，主程序会重试主动拔钩

    阶段六（反横跳闸门，v0.6.6）断言 1 条：
     15. 客户端每 200ms 就把无边框全屏抢回来时，外部纠正必须认输停手

    阶段七（采集栈监控 + 自动导出，v0.6.12）断言 2 条：
     16. 主程序常驻监控发现"某进程加载了 ExdDtDup.dll"
     17. 这次命中触发一次静默自动导出，且诊断包 snapshot.txt 含"== 采集栈快照 =="与该命中

    阶段八（P0 空过程替换的回归闸门，v0.6.13/v0.6.15）断言 4 条：
     18. 远志自己装键盘钩子（hMod=自身模块）时被换成"空过程"且返回合法 HHOOK
     19. 模拟客户端拿到合法句柄后**继续存活**（旧行为：返回 NULL → 8 毫秒内退出）
     20. hMod=NULL 的**线程内**钩子同样被换成空过程（走"回调地址反查模块"那条判据，
         v0.6.15 补：这条分支此前从未被任何测试走到过）
     21. 该模式下模拟客户端也继续存活
         做法：把模拟目标复制到路径含 GZYZ 的目录里（命中 IsYuanzhiInstallPath 的
         子串判据，不需要在生产代码里开后门），用 --hook / --hook-thread 让它
         "自己装钩子 + 拿 NULL 就自杀"；两个模式都是**启动 8 秒后**才装钩子
         （现场那次是"注入之后"才装的）。注意模拟目标用 %p 打印句柄（不带 0x 前缀）。

    阶段九（消息层钩子开关 BlockMessageHooks，v0.6.14）断言 2 条：
     22. BlockMessageHooks=1（默认）：键盘与消息层两类钩子**都被**换成空过程
     23. BlockMessageHooks=0：键盘钩子照旧被换掉，消息层那条**放行**
         ——守的是"case 标签不构成作用域"那类实现错误，以及"控制位/ini 值"这条下发链
         （v0.6.13 那两处 bug 就是这一步抓出来的）。

    阶段十（探针 --watch 的触发式采集，v0.6.15）断言 2 条：
     24. 起一个常驻探针 + 让某个进程加载改名成 ExdDtDup.dll 的替身 →
         探针必须落一份 -watch-capture 报告，且报告里命中那个进程
     25. 探针按 --duration 正常退出（watch 日志里有"watch 结束：共落盘 N 份报告"）
         ——现场"教师监视时谁在抓屏"的主力采集器就是它（阶段七只覆盖了主程序那一半）。

    阶段十一（考试/测验强信号熔断，v0.6.16）断言 2 条：
     26. 强信号命中（YZSimTarget --exam 造一个标题含"考试"的窗口、且进程在远志路径下）时
         必须立刻熔断：日志出现"检测到考试/测验模式"，并且功能**真的停了**
         ——模拟目标的广播窗口不会被窗口化（状态文件仍是 fullscreen=1）
     27. 对照组：EnableExamGuard=0 时钩子侧必须记录"考试模式检测已按配置关闭"，
         且功能恢复（窗口化照常生效，fullscreen=0）。这条同时守住"考试守护位"这条控制位
         下发链——v0.6.13/v0.6.14 两个 bug 都出在同类链上，而考试守护是项目的硬边界。
         注意：注入到配置到达之间约 1 秒内，钩子侧仍是 fail-closed 的默认守护开，
         那一段里本来就会有一条熔断日志（设计行为，不是 bug）。

    阶段十二（YZSysRun 令牌复制自检，v0.6.17）断言 2 条：
     28. YZSysRun 能把子进程以 SYSTEM 起起来（它自己写的 txt 里有
         "NT AUTHORITY\SYSTEM … 16384"）
     29. 子进程留在**当前交互会话**且是系统完整性（"已启动 … 会话=<当前会话> … 16384"）
         ——会话错到 Session 0 的话，现场探针/窗口枚举全部失真，这条专门防它。
         它是现场部署的强制启动路径（学生端跑 SYSTEM，主程序必须同级完整性），
         此前没有任何断言覆盖。

    阶段十三（免注入路径下的假全屏，v0.6.18）断言 2 条：
     30. AutoInject=0 + ExternalWindowFix=1 + 假全屏（Flags=36）时，跨进程纠正也要能做出
         假全屏：状态文件出现 fullscreen=1 且 topmost=0，日志出现"外部窗口纠正(假全屏)"
     31. 该路径下不能给窗口加标题栏（外观仍是"全屏广播"的样子）
         ——阶段三只覆盖了进程内假全屏、阶段二只覆盖了跨进程窗口化，这条组合是机房
         "注入不进去"时的兜底手段，此前没有断言。

    阶段十四（身份不足时的判读，v0.6.19）断言 2 条：
     32. 目标是 SYSTEM、主程序是管理员（高完整性）时，跨进程窗口写会被 UIPI 拒绝：
         日志出现"外部窗口纠正…失败"（不静默失败），且目标仍是 fullscreen=1
     33. 换成 YZSysRun 以 SYSTEM 启动主程序后，同一操作**成功**：目标状态由 fullscreen=1
         变成 fullscreen=0，且"外部窗口纠正: 0x… -> x,y wxh"出现在**两份日志之一**
         （SYSTEM 主程序的日志在 `C:\Windows\Temp\YZTrainer\yzt.log`，不在学生账户那份里
         ——本轮实测确认，两条通道都查）
         ——这两条就是 09-23 误判 / 09-29 判决的本地复现：0x5 是 UIPI 一致性检查，
         不是"驱动剥夺"，同级完整性就放行。

    阶段十五（冻结开启时"别把自己拍进去"，v0.6.20）断言 2 条：
     34. 防监视开启时主界面自动收进托盘：进程的 MainWindowHandle 归 0，
         且日志出现"防监视已开启：主界面已收入托盘并从屏幕捕获中排除"
     35. 主窗口的显示亲和性确实是 WDA_EXCLUDEFROMCAPTURE（0x11）：断言查主程序**进程内**
         读回并写进日志的那行（`进程内读回=0x11`），并要求它确实冻了画面（日志有"已冻结画面"）。
         注意 GetWindowDisplayAffinity **跨进程读回来总是 0**（2026-10-11 实测），所以脚本里
         的进程外枚举只作诊断输出、不作断言。
         本机实测（2026-10-11）：该亲和性在 GDI BitBlt 与 DXGI 桌面复制两条抓屏路径上
         都会排除窗口，露出的是窗口后面的桌面内容而不是黑块；见 开发记录.md §26。

    阶段十六（DXGI 桌面复制取证，v0.6.21）断言 2 条：
     36. `TraceDesktopDuplication=1` 时日志出现"[DXGI 取证] 建立桌面复制成功"
         （DuplicateOutput 被接管——只计数，不改任何返回值）
     37. 日志出现"[DXGI 取证] 周期 取帧统计"，且模拟目标自己记到"dup: 完成 取到帧=N"（N>0）
         这条钩子默认关闭，是给机房判"教师监视/广播到底有没有走桌面复制"用的；
         尤其要看清学生机上的 DuplicateOutput 会不会也被"接收广播"用到。

    阶段十七（探针跨架构枚举，v0.6.22）断言 2 条：
     38. **x64** 探针能在 32 位目标上枚举模块并判出"采集层=DXGI"（依赖 TH32CS_SNAPMODULE32）；
         漏了那个标志不会报错、报告照样生成，只是模块列表为空——现场会误判"没有采集组件"，
         整趟数据作废，所以必须钉住。
     39. x64 探针的"全进程采集组件扫描"也命中该 32 位目标（另一处枚举代码）。

    阶段十八（探针的保护级别判读，v0.6.23）断言 2 条：
     40. 仅因"采集组件扫描"晋级的进程（放在不带远志路径特征的目录里）确实进了
         "远志相关进程"表——这类进程正是现场最想看的"真正持 DuplicateOutput 的那个"
     41. 它的"进程保护"列必须是"无/无保护"，**不能**是 `PPL/WinTcbLight`
         （v0.6.23 修的 bug：级别只在 isYuanzhi 时查，而晋级发生在之后，于是级别留在初值 0，
         而 0 在枚举里等于 WINTCB_LIGHT，现场会读成"这个抓屏进程受 PPL 保护"）

    阶段十九（x64 探针 --watch 跨架构，v0.6.24）断言 2 条：
     42. **x64** 探针的 `--watch`（现场采"教师监视时谁在抓屏"的主力通道）在 32 位目标上
         触发落盘并命中采集组件——它和一次性扫描是两条代码路径（常驻轮询 + 触发判定 + 落盘）
     43. 那份 watch 报告里该 pid 的"图形采集栈"行判为"采集层=DXGI"（跨架构下判词也成立）

    阶段二十（InjectMethod=1 消息钩子注入，v0.6.25）断言 3 条：
     44. `InjectMethod=1` 时日志出现"已通过消息钩子注入 HHOOK=…"（拿到合法句柄）
     45. 窗口化照常生效（目标状态 fullscreen=0）——说明引擎真的被映射进目标并跑起来了
     46. 该阶段里**没有**"已注入目标进程 pid="（=1 时不该再去尝试远程线程注入）
         注意这条路的固有副作用（README 已写明）：WH_GETMESSAGE + dwThreadId=0 是会话级全局钩子，
         DLL 会被映射进本会话所有取消息的 GUI 进程（非目标进程里引擎自检后放弃）。

    阶段二十一（界面/热键那条开关链，v0.6.26）断言 3 条：
     47/48. `CmdSetFlag` 对四个功能位是**只发 `YZ_CMD_SET_*` opcode、不发整份配置**，
         所以用户勾复选框 / 按热键走的是钩子侧 `EngineSetFlag` 那条链，此前从没被测过。
         这里用**确定性驱动**：复选框走 `WM_COMMAND` + `BM_SETCHECK`（与鼠标点击同一条代码路径），
         热键走直接投递 `WM_HOTKEY`（与系统投递同一个处理器）——不依赖输入队列。
         断言：勾选后抓帧哈希停住且 ini 出现防监视位；取消后哈希又变化。
     49. `WM_HOTKEY(F12)` 真的能隐藏主界面、再发一次恢复显示，
         钉住 v0.6.26 修的"F12 原来只显示、不隐藏"那个 bug。
     另：本机实测 Ctrl+Alt+F9 已被别的程序占用、RegisterHotKey 直接失败（err=1409），
         而老代码静默——v0.6.26 起注册失败会记 WARN（产品改动）。

    ── 断言写法约定（血泪换来的，新加阶段请照做）────────────────────────────
    * 日志里的句柄/地址用 %p 打印，**不带 0x 前缀**（"HHOOK=002A0E26"）。凡是要匹配它们，
      一律写成 `(?:0x)?0*[1-9A-Fa-f]`——v0.6.13 阶段八与 v0.6.25 阶段二十各栽过一次。
    * 只看**本阶段新增**的日志段：用 Get-LogLength 记起点 + Read-LogTail 读增量，
      不要 grep 整个 yzt.log（它是追加写的、跨次运行共用，会被旧行满足）。
      v0.6.25 起这两个助手也认 <日志>-1.log：产品在 yzt.log 超过 2MB 后会改用 yzt-1.log，
      一次完整回归必然越过这条线，只读主日志会让后续阶段读到"空的新增日志"而误报失败。
    * 环境形状必须与产品判据一致：要不要放到"路径含 GZYZ"的目录里（IsYuanzhiInstallPath）、
      进程什么时候被杀、产品把日志写到哪个文件（UiAppendLog 只进界面、不落文件；
      以 SYSTEM 启动的主程序日志在 C:\Windows\Temp\YZTrainer\）。
      这三点各导致过一次"红的是测试、不是产品"。

    运行前请关闭其它 YZTrainer 实例（含改名副本）：主程序用全局互斥体防重入，
    别的实例在跑时本脚本启动的那份会静默退出，测试结果会变成假失败。

    注意：测试结束时脚本会强杀模拟目标与主程序，因此不要在有真实远志客户端运行时使用。
#>
param(
    [string]$DistDir,
    [int]$StartupWaitSeconds = 6,
    # 阶段二（免注入外部纠正）不需要管理员权限；阶段一需要。给出这个开关是为了
    # 能在普通账户下单独验证外部纠正（例如用 asInvoker 的测试副本跑）。
    [switch]$AllowUnelevated
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $DistDir) { $DistDir = Join-Path $root 'dist\Win32' }

$trainer = Join-Path $DistDir 'YZTrainer.exe'
$sim     = Join-Path $DistDir 'YZSimTarget.exe'
foreach ($f in @($trainer, $sim)) {
    if (-not (Test-Path $f)) { throw "缺少文件: $f （请先运行 build.ps1）" }
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    if ($AllowUnelevated) {
        Write-Warning '当前不是管理员：阶段一（注入路径）可能失败，阶段二（免注入外部纠正）不受影响。'
    } else {
        throw '请以管理员身份运行本测试脚本（否则 YZTrainer 无法注入模拟目标）。只想验证免注入路径时加 -AllowUnelevated。'
    }
}

$stamp     = Get-Date -Format 'yyyyMMdd-HHmmss'
$stateFile = Join-Path $DistDir 'sim_state.txt'
$capFile   = Join-Path $DistDir 'sim_capture.csv'
$iniFile   = Join-Path $DistDir 'YZTrainer.ini'
# 主程序日志：阶段二起多处断言都只按「本阶段新增的行」匹配（见 Get-LogLength/Read-LogTail）
$appLog    = Join-Path (Join-Path $env:TEMP 'YZTrainer') 'yzt.log'

# 备份已有产物（不删除任何文件）
foreach ($f in @($stateFile, $capFile, $iniFile)) {
    if (Test-Path $f) { Move-Item -LiteralPath $f -Destination "$f.bak-$stamp" -Force }
}

# 测试配置：窗口化 + 键鼠解锁 + 防监视（Flags = 1 | 4 | 8 = 13）
$iniLines = @(
    '[General]',
    'Flags=13',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=1',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=0',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $iniLines -Encoding utf8

$simProc = $null
$appProc = $null
$result  = New-Object System.Collections.Generic.List[string]

# 断言只看"本阶段新产生的日志行"：yzt.log 是追加写的，跨阶段、跨次运行共用同一个文件，
# 直接对整个文件 Select-String 会被上一次运行的旧行骗过（v0.6.13 的负向 A/B 就踩到了）。
# 用法：阶段开始前 $mark = Get-LogLength $appLog，断言处用 Read-LogTail $appLog $mark。
# 日志轮转感知（v0.6.25）：产品在 yzt.log 超过 2MB 后改用 yzt-1.log（README 有写），
# 而回归跑到中后段必然会超过 2MB——只读 yzt.log 的话，之后的阶段会读到"空的新增日志"，
# 断言误报失败（第一次暴露：阶段二十，功能其实是通的）。所以：
#   Get-LogLength 返回一个"各文件长度快照"字符串（调用方只把它原样传回 Read-LogTail）；
#   Read-LogTail 对主日志与轮转日志各读自己新增的那一段，并把"文件被重建/变短"当作从头读。
function Get-RelatedLogPaths([string]$Path) {
    if ([string]::IsNullOrEmpty($Path)) { return @() }
    $pairs = New-Object System.Collections.Generic.List[string]
    $pairs.Add($Path)
    # 轮转文件：<base>-1.log（产品按这个命名）
    $dir  = [System.IO.Path]::GetDirectoryName($Path)
    $base = [System.IO.Path]::GetFileNameWithoutExtension($Path)
    $ext  = [System.IO.Path]::GetExtension($Path)
    if (-not [string]::IsNullOrEmpty($dir)) {
        $pairs.Add((Join-Path $dir ("{0}-1{1}" -f $base, $ext)))
    }
    return $pairs
}
function Get-LogLength([string]$Path) {
    $snap = @()
    foreach ($f in (Get-RelatedLogPaths $Path)) {
        $len = -1
        if (Test-Path -LiteralPath $f) { $len = (Get-Item -LiteralPath $f).Length }
        $snap += ("{0}={1}" -f $f, $len)
    }
    return ($snap -join ';')
}
function Read-LogTail([string]$Path, [string]$Mark) {
    if ([string]::IsNullOrEmpty($Path)) { return '' }
    # 兼容旧调用（传字节数）与不含 '=' 的 mark
    $marks = @{}
    if (-not [string]::IsNullOrEmpty($Mark) -and $Mark.Contains('=')) {
        foreach ($part in ($Mark -split ';')) {
            $kv = $part -split '=', 2
            if ($kv.Count -eq 2) { $marks[$kv[0]] = [long]$kv[1] }
        }
    }
    $out = New-Object System.Text.StringBuilder
    foreach ($f in (Get-RelatedLogPaths $Path)) {
        if (-not (Test-Path -LiteralPath $f)) { continue }
        $len = (Get-Item -LiteralPath $f).Length
        $from = 0
        if ($marks.ContainsKey($f) -and $marks[$f] -ge 0 -and $marks[$f] -le $len) { $from = $marks[$f] }
        if ($len -le $from) { continue }
        $fs = [System.IO.File]::Open($f, [System.IO.FileMode]::Open,
                                     [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            [void]$fs.Seek($from, [System.IO.SeekOrigin]::Begin)
            $sr = New-Object System.IO.StreamReader($fs)
            [void]$out.Append($sr.ReadToEnd())
        }
        finally { $fs.Dispose() }
    }
    return $out.ToString()
}

try {
    Write-Host '启动模拟目标 ...'
    $simProc = Start-Process -FilePath $sim -PassThru
    Start-Sleep -Seconds 3

    $state1 = Get-Content -LiteralPath $stateFile -ErrorAction SilentlyContinue
    Write-Host "模拟目标状态: $state1"
    if ($state1 -match 'fullscreen=1') { $result.Add('PASS  模拟目标已进入全屏置顶') }
    else { $result.Add('FAIL  模拟目标未进入全屏') }

    Write-Host '启动 YZTrainer ...'
    $mark1 = Get-LogLength $appLog
    $appProc = Start-Process -FilePath $trainer -PassThru
    Start-Sleep -Seconds $StartupWaitSeconds

    $state2 = Get-Content -LiteralPath $stateFile -ErrorAction SilentlyContinue
    Write-Host "窗口化后状态: $state2"
    if ($state2 -match 'fullscreen=0') { $result.Add('PASS  广播窗口已被窗口化') }
    else { $result.Add('FAIL  广播窗口仍是全屏（注入或窗口化未生效）') }

    if ($state2 -match 'style=0x([0-9A-Fa-f]{8})') {
        $style = [Convert]::ToUInt32($Matches[1], 16)
        if (($style -band 0x00C00000) -eq 0x00C00000) { $result.Add('PASS  窗口已带标题栏(WS_CAPTION)') }
        else { $result.Add('FAIL  窗口样式未包含 WS_CAPTION') }
    }

    # v0.6.14：不受保护的目标必须仍走远程线程注入，且注入预检不能误判。这两条守的是
    # v0.6.13 新增的 VmOperationStripped()：它一旦误报，所有机器都会被无谓地降级到
    # 消息钩子（消息钩子会把 DLL 映射进本会话所有 GUI 进程）。
    $seg1 = Read-LogTail $appLog $mark1
    if ($seg1 -match '已注入目标进程') { $result.Add('PASS  不受保护的目标走的仍是远程线程注入') }
    else { $result.Add('FAIL  没有看到“已注入目标进程”（可能被注入预检误判后降级了）') }

    if ($seg1 -notmatch '缺 PROCESS_VM_OPERATION') { $result.Add('PASS  注入预检没有误判不受保护的目标') }
    else { $result.Add('FAIL  注入预检把不受保护的目标判成了“句柄权限被削”') }

    if (Test-Path $capFile) {
        $lines  = Get-Content -LiteralPath $capFile | Select-Object -Last 6
        $hashes = $lines | ForEach-Object { ($_ -split ',')[1] } | Where-Object { $_ }
        $unique = $hashes | Select-Object -Unique
        Write-Host ("最近抓帧哈希: " + ($hashes -join ', '))
        if ($hashes.Count -ge 3 -and @($unique).Count -eq 1) { $result.Add('PASS  防监视生效：教师端看到的画面被冻结') }
        else { $result.Add('FAIL  画面未冻结（哈希仍在变化）') }
    } else {
        $result.Add('FAIL  没有生成 sim_capture.csv')
    }
}
finally {
    if ($simProc -and -not $simProc.HasExited) { Stop-Process -Id $simProc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($appProc -and -not $appProc.HasExited) { Stop-Process -Id $appProc.Id -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# 阶段二：免注入兜底（ExternalWindowFix）
# AutoInject=0 让主程序完全不注入，窗口化只能靠跨进程改样式完成；
# 模拟目标每 3 秒抢回全屏、外部纠正每秒一次，所以这里轮询等待而不是只看一眼。
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段二：免注入外部窗口纠正 ===='

foreach ($f in @($stateFile, $capFile)) {
    if (Test-Path $f) { Move-Item -LiteralPath $f -Destination "$f.bak2-$stamp" -Force }
}

$phase2Ini = @(
    '[General]',
    'Flags=5',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=0',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=1',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase2Ini -Encoding utf8

$sim2Proc = $null
$app2Proc = $null

try {
    $sim2Proc = Start-Process -FilePath $sim -PassThru
    Start-Sleep -Seconds 3

    $mark2 = Get-LogLength $appLog
    $app2Proc = Start-Process -FilePath $trainer -PassThru

    $fixed = $false
    $state2 = ''
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 700
        $state2 = Get-Content -LiteralPath $stateFile -ErrorAction SilentlyContinue
        if ($state2 -match 'fullscreen=0') { $fixed = $true; break }
    }

    Write-Host "免注入窗口化后状态: $state2"
    if ($fixed) { $result.Add('PASS  免注入模式下窗口仍被窗口化（外部纠正生效）') }
    else { $result.Add('FAIL  免注入模式下窗口仍是全屏（外部纠正未生效）') }

    if ($fixed -and $state2 -match 'style=0x([0-9A-Fa-f]{8})') {
        $style2 = [Convert]::ToUInt32($Matches[1], 16)
        if (($style2 -band 0x00C00000) -eq 0x00C00000) { $result.Add('PASS  外部纠正后的窗口带标题栏(WS_CAPTION)') }
        else { $result.Add('FAIL  外部纠正后的窗口样式不含 WS_CAPTION') }
    }

    $appLog = Join-Path (Join-Path $env:TEMP 'YZTrainer') 'yzt.log'
    if ((Read-LogTail $appLog $mark2) -match '外部窗口纠正') {
        $result.Add('PASS  主程序日志里出现“外部窗口纠正”记录')
    } else {
        $result.Add('WARN  未在 yzt.log 里找到“外部窗口纠正”记录（不影响窗口化结论）')
    }
}
finally {
    if ($sim2Proc -and -not $sim2Proc.HasExited) { Stop-Process -Id $sim2Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app2Proc -and -not $app2Proc.HasExited) { Stop-Process -Id $app2Proc.Id -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# 阶段三：假全屏（窗口化关、假全屏开）
# 期望：窗口仍铺满整屏、没有标题栏，但已不置顶——这样 Alt+Tab 切过去的窗口能盖在广播上。
# 模拟目标每 3 秒抢回全屏并重新置顶，所以同样要轮询等待。
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段三：假全屏 ===='

foreach ($f in @($stateFile, $capFile)) {
    if (Test-Path $f) { Move-Item -LiteralPath $f -Destination "$f.bak3-$stamp" -Force }
}

$phase3Ini = @(
    '[General]',
    'Flags=36',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=1',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=0',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase3Ini -Encoding utf8

$sim3Proc = $null
$app3Proc = $null

try {
    $sim3Proc = Start-Process -FilePath $sim -PassThru
    Start-Sleep -Seconds 3
    $app3Proc = Start-Process -FilePath $trainer -PassThru

    $fakeOk = $false
    $state3 = ''
    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 700
        $state3 = Get-Content -LiteralPath $stateFile -ErrorAction SilentlyContinue
        if ($state3 -match 'fullscreen=1' -and $state3 -match 'topmost=0') { $fakeOk = $true; break }
    }
    Write-Host "假全屏状态: $state3"

    if ($fakeOk) { $result.Add('PASS  假全屏：窗口仍全屏且已取消置顶') }
    else { $result.Add('FAIL  假全屏未生效（窗口仍置顶或不再全屏）') }

    if ($state3 -match 'style=0x([0-9A-Fa-f]{8})') {
        $style3 = [Convert]::ToUInt32($Matches[1], 16)
        if (($style3 -band 0x00C00000) -ne 0x00C00000) { $result.Add('PASS  假全屏下没有加标题栏（外观仍是全屏广播）') }
        else { $result.Add('FAIL  假全屏下被加了标题栏') }
    }
}
finally {
    if ($sim3Proc -and -not $sim3Proc.HasExited) { Stop-Process -Id $sim3Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app3Proc -and -not $app3Proc.HasExited) { Stop-Process -Id $app3Proc.Id -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# 阶段四：教师端远程执行审计（YZSimTarget --exec 模拟教师端下发的 CreateProcess）
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段四：远程执行审计 ===='

$auditFile = Join-Path (Join-Path $env:TEMP 'YZTrainer') 'remote-exec.log'
if (Test-Path $auditFile) { Move-Item -LiteralPath $auditFile -Destination "$auditFile.bak4-$stamp" -Force }

$phase4Ini = @(
    '[General]',
    'Flags=5',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=1',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=0',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase4Ini -Encoding utf8

$sim4Proc = $null
$app4Proc = $null

try {
    $sim4Proc = Start-Process -FilePath $sim -ArgumentList '--exec' -PassThru
    Start-Sleep -Seconds 2
    $app4Proc = Start-Process -FilePath $trainer -PassThru
    Start-Sleep -Seconds 12

    $auditLines = if (Test-Path $auditFile) { Get-Content -LiteralPath $auditFile } else { @() }
    Write-Host ("审计文件行数: " + @($auditLines).Count)
    if (@($auditLines).Count -gt 0) { Write-Host ("最后一行: " + ($auditLines | Select-Object -Last 1)) }

    if (@($auditLines | Where-Object { $_ -match 'kind=CreateProcessW' }).Count -gt 0) {
        $result.Add('PASS  远程执行审计记录了 CreateProcessW')
    } else {
        $result.Add('FAIL  remote-exec.log 里没有 CreateProcessW 记录')
    }

    if (@($auditLines | Where-Object { $_ -match 'kind=CreateProcessW.*origin=remote' }).Count -gt 0) {
        $result.Add('PASS  审计把外部进程标为 origin=remote')
    } else {
        $result.Add('FAIL  审计记录里没有 origin=remote 的条目')
    }
}
finally {
    if ($sim4Proc -and -not $sim4Proc.HasExited) { Stop-Process -Id $sim4Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app4Proc -and -not $app4Proc.HasExited) { Stop-Process -Id $app4Proc.Id -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# 阶段五：主动拔钩的"晚加载"重试（v0.6.3 / P1.3）
# 模拟"注入已经发生、远志的钩子模块随后才被加载"：YZSimTarget 用 --load 在启动 4 秒后
# 加载一个名叫 ExdHooks.dll 的替身 DLL（用系统自带的 version.dll 改名，避免依赖远志文件）。
# 期望：主程序在 2 秒内重新发起一轮主动拔钩，日志里能看到
#   NativeUnhook: 目标模块 ExdHooks.dll 已加载，开始尝试
#   （随后因为替身没有那个导出，跟一条"找不到导出 … 跳过"）
# 这条断言直接盯着"注入与模块加载的时序竞争"，是 v0.6.3 修 P1.3 的回归闸门。
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段五：主动拔钩的晚加载重试 ===='

$stubDir  = Join-Path $env:TEMP 'yzt-lateload'
$stubDll  = Join-Path $stubDir 'ExdHooks.dll'
$stubSrc  = @(
    (Join-Path $env:WINDIR 'SysWOW64\version.dll'),
    (Join-Path $env:WINDIR 'System32\version.dll')
) | Where-Object { Test-Path $_ } | Select-Object -First 1

$phase5Ini = @(
    '[General]',
    'Flags=5',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=1',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=0',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase5Ini -Encoding utf8

$sim5Proc = $null
$app5Proc = $null

try {
    if (-not $stubSrc) {
        $result.Add('WARN  系统里没有 version.dll 可做替身，跳过阶段五')
    }
    else {
        New-Item -ItemType Directory -Force -Path $stubDir | Out-Null
        Copy-Item -LiteralPath $stubSrc -Destination $stubDll -Force

        $sim5Proc = Start-Process -FilePath $sim -ArgumentList '--load', $stubDll -PassThru
        Start-Sleep -Seconds 2
        $mark5 = Get-LogLength $appLog
        $app5Proc = Start-Process -FilePath $trainer -PassThru
        Start-Sleep -Seconds 14

        $seg5     = Read-LogTail $appLog $mark5
        $retryHit = $seg5 -match 'NativeUnhook: 目标模块 ExdHooks\.dll 已加载，开始尝试'
        $skipHit  = $seg5 -match '找不到导出 UnSetExdHooks'
        Write-Host ("晚加载重试命中: " + $retryHit + " ; 找不到导出(替身预期): " + $skipHit)

        if ($retryHit) { $result.Add('PASS  钩子模块晚于注入出现时，主动拔钩会重试') }
        else { $result.Add('FAIL  钩子模块晚加载后没有发起新一轮主动拔钩') }
    }
}
finally {
    if ($sim5Proc -and -not $sim5Proc.HasExited) { Stop-Process -Id $sim5Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app5Proc -and -not $app5Proc.HasExited) { Stop-Process -Id $app5Proc.Id -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# 阶段六：反横跳闸门（v0.6.6）
# 模拟"客户端立刻响应我们的改动"：YZSimTarget --fight 每 200ms 把无边框全屏抢回来。
# 期望：外部纠正连续几次短间隔抢回后**认输停手**，日志出现"已对该窗口停手"，
#       而不是跟着客户端每秒十几次地互相拉扯（那在 Win11 下就是放大缩小横跳）。
# 用 AutoInject=0 保证走的是跨进程纠正这条路径（注入路径会被进程内 Hook 接管）。
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段六：反横跳闸门 ===='

foreach ($f in @($stateFile, $capFile)) {
    if (Test-Path $f) { Move-Item -LiteralPath $f -Destination "$f.bak6-$stamp" -Force }
}

$phase6Ini = @(
    '[General]',
    'Flags=5',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=0',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=1',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase6Ini -Encoding utf8

$sim6Proc = $null
$app6Proc = $null

try {
    $sim6Proc = Start-Process -FilePath $sim -ArgumentList '--fight' -PassThru
    Start-Sleep -Seconds 2
    $mark6 = Get-LogLength $appLog
    $app6Proc = Start-Process -FilePath $trainer -PassThru
    Start-Sleep -Seconds 12

    $gaveUp = (Read-LogTail $appLog $mark6) -match '已对该窗口停手'
    Write-Host ("认输停手日志命中: " + $gaveUp)
    if ($gaveUp) { $result.Add('PASS  客户端高频抢回全屏时，外部纠正会认输停手（不再横跳）') }
    else { $result.Add('FAIL  高频抢回场景下外部纠正没有停手（仍会来回横跳）') }
}
finally {
    if ($sim6Proc -and -not $sim6Proc.HasExited) { Stop-Process -Id $sim6Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app6Proc -and -not $app6Proc.HasExited) { Stop-Process -Id $app6Proc.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host ''
# ---------------------------------------------------------------------------
# 阶段七：采集栈监控 + 自动导出（v0.6.12）
# 动机：教师监视/缩略图时厂商会在某个进程里加载 ExdDtDup.dll（DXGI 桌面复制）或拉起
#       ScreenRecoder.exe（GDI 抓屏 helper）——"冻结画面"钩哪条路由这件事决定，而它只在
#       教师动手的那几秒里出现，靠人在现场跑探针赌不起（10-09 那次就赌输了）。
# 做法：把一个改名的版本 DLL（version.dll → ExdDtDup.dll）用 YZSimTarget --load 载入，
#       模拟"某进程加载了 DXGI 采集组件"；先让它存在，再启动主程序，第一轮扫描就该命中。
# 断言：日志出现"采集栈监控: PID=… 出现采集组件 ExdDtDup.dll"，且自动导出的诊断包里
#       snapshot.txt 含"== 采集栈快照 =="与该命中。
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段七：采集栈监控 ===='

$ddStubDir = Join-Path $env:TEMP 'yzt-capture'
$ddStub    = Join-Path $ddStubDir 'ExdDtDup.dll'
# 替身 DLL 的位数必须与模拟目标一致：dist\x64 用 System32、dist\Win32 用 SysWOW64
$ddStubSrc = if ($DistDir -match 'x64$') { Join-Path $env:WINDIR 'System32\version.dll' }
             else { Join-Path $env:WINDIR 'SysWOW64\version.dll' }
if (-not (Test-Path $ddStubSrc)) { $ddStubSrc = Join-Path $env:WINDIR 'System32\version.dll' }

$phase7Ini = @(
    '[General]',
    'Flags=5',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=0',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=0',
    'AutoExportOnCapture=1',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase7Ini -Encoding utf8

$sim7Proc = $null
$app7Proc = $null

try {
    if (-not (Test-Path $ddStubSrc)) {
        $result.Add('WARN  系统里没有 version.dll 可做替身，跳过阶段七')
    }
    else {
        New-Item -ItemType Directory -Force -Path $ddStubDir | Out-Null
        Copy-Item -LiteralPath $ddStubSrc -Destination $ddStub -Force

        $stage7Start = Get-Date
        $sim7Proc = Start-Process -FilePath $sim -ArgumentList '--load', $ddStub -PassThru
        Start-Sleep -Seconds 7
        $mark7 = Get-LogLength $appLog
        $app7Proc = Start-Process -FilePath $trainer -PassThru
        Start-Sleep -Seconds 10

        $capHit = (Read-LogTail $appLog $mark7) -match '采集栈监控: PID=\d+ .*ExdDtDup\.dll'
        Write-Host ("采集栈监控命中: " + $capHit)
        if ($capHit) { $result.Add('PASS  常驻监控发现进程加载了 ExdDtDup.dll') }
        else { $result.Add('FAIL  常驻监控没有发现 ExdDtDup.dll 被加载') }

        $snapHit = $false
        foreach ($d in (Get-ChildItem -LiteralPath $DistDir -Directory -Filter 'diag-*-auto' -ErrorAction SilentlyContinue |
                        Where-Object { $_.LastWriteTime -ge $stage7Start })) {
            $snap = Join-Path $d.FullName 'snapshot.txt'
            if (-not (Test-Path $snap)) { continue }
            if ((Select-String -LiteralPath $snap -Pattern '== 采集栈快照 ==' -Quiet) -and
                (Select-String -LiteralPath $snap -Pattern 'ExdDtDup\.dll' -Quiet)) { $snapHit = $true; break }
        }
        Write-Host ("诊断包采集栈快照命中: " + $snapHit)
        if ($snapHit) { $result.Add('PASS  诊断包快照里带采集栈快照与命中记录') }
        else { $result.Add('FAIL  诊断包快照里没有采集栈命中记录') }
    }
}
finally {
    if ($sim7Proc -and -not $sim7Proc.HasExited) { Stop-Process -Id $sim7Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app7Proc -and -not $app7Proc.HasExited) { Stop-Process -Id $app7Proc.Id -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# 阶段八：P0"空过程替换"的回归闸门（v0.6.13）
# 现场证据（2026-10-09）：v0.6.10 把远志的 SetWindowsHookEx 拦成 NULL 时，Yistart.exe 8 毫秒内
# 退出、守卫 4 秒后拉起，形成十秒级重启循环（电子点名窗口因此立不住）。v0.6.11 改成"用本 DLL 的
# 空过程顶替、返回合法 HHOOK"。这条修复此前**没有任何本地防线**，本阶段补上。
# 做法：把模拟目标复制到**路径含 GZYZ** 的目录（命中 IsYuanzhiInstallPath 的子串判据，不需要在
# 生产代码里开后门），用 --hook 让它像厂商那样自己装钩子、拿 NULL 就自杀。
# 注意 --hook 是**启动 8 秒后**才装钩子：现场那次是注入之后才装的；启动时立刻装会走系统原生
# 路径，完全绕开我们的钩子（第一版就是这么写的，实测两边都不命中）。
# 断言：
#   16. 日志出现"已用空过程替换远志键盘/鼠标钩子"，且返回的 hhook 非 0
#   17. 模拟客户端在替换之后仍然存活（旧行为是 8 毫秒内退出 → 本断言会红）
#      注意模拟目标用 %p 打印句柄（不带 0x 前缀），匹配两种写法才不会被格式坑到
# 负向验证（人工做一次即可）：把 hooks_input.cpp 的 ReplaceWithPassthrough 暂时改成直接
# return nullptr，重编后本阶段必须失败——证明这条闸门真的会拦人。
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段八：P0 空过程替换（客户端自杀回归）===='

$gzyzDir     = Join-Path $env:TEMP 'GZYZ-yzsim'
$gzyzSim     = Join-Path $gzyzDir 'YZSimTarget.exe'
$gzyzHookLog = Join-Path $gzyzDir 'sim_log.txt'

$phase8Ini = @(
    '[General]',
    'Flags=5',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=1',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=0',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase8Ini -Encoding utf8

$sim8Proc = $null
$app8Proc = $null

try {
    New-Item -ItemType Directory -Force -Path $gzyzDir | Out-Null
    if (Test-Path $gzyzHookLog) { Move-Item -LiteralPath $gzyzHookLog -Destination "$gzyzHookLog.bak8-$stamp" -Force }
    Copy-Item -LiteralPath $sim -Destination $gzyzSim -Force

    # 模拟目标在启动 8 秒后才装钩子（现场那次是注入之后才装的）：先让它起来，再启动主程序
    # 完成注入，最后等它装钩子并观察是否存活。
    $sim8Proc = Start-Process -FilePath $gzyzSim -ArgumentList '--hook' -PassThru
    Start-Sleep -Seconds 4
    $mark8 = Get-LogLength $appLog
    $app8Proc = Start-Process -FilePath $trainer -PassThru
    Start-Sleep -Seconds 14

    $seg8      = Read-LogTail $appLog $mark8
    $replaced  = $seg8 -match '已用空过程替换远志键盘/鼠标钩子'
    $legalHook = $seg8 -match '已用空过程替换远志键盘/鼠标钩子.*hhook=0x0*[1-9A-Fa-f]'
    $simAlive = $sim8Proc -and (-not $sim8Proc.HasExited)
    $simHookLine = if (Test-Path $gzyzHookLog) {
        (Get-Content -LiteralPath $gzyzHookLog | Where-Object { $_ -match 'hook-install' } | Select-Object -Last 1)
    } else { '' }
    Write-Host ("替换命中: " + $replaced + " ; 合法句柄: " + $legalHook + " ; 客户端存活: " + $simAlive)
    Write-Host ("模拟客户端钩子日志: " + $simHookLine)

    if ($replaced -and $legalHook) { $result.Add('PASS  远志钩子安装被换成空过程且返回合法 HHOOK') }
    else { $result.Add('FAIL  没有看到“已用空过程替换…且 hhook 非 0”（退回拦截，客户端会自杀）') }

    if ($simAlive -and $simHookLine -match 'hhook=(?:0x)?0*[1-9A-Fa-f]') { $result.Add('PASS  模拟客户端拿到合法句柄后继续存活（不再十秒一跳）') }
    else { $result.Add('FAIL  模拟客户端没能存活/没拿到合法句柄——P0 修复被改回去了') }

}
finally {
    if ($sim8Proc -and -not $sim8Proc.HasExited) { Stop-Process -Id $sim8Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app8Proc -and -not $app8Proc.HasExited) { Stop-Process -Id $app8Proc.Id -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# 阶段八（第二段）：--hook-thread —— hMod=NULL + 本线程 tid（v0.6.15）
# 走的是 ShouldBlockHook 里"按回调地址反查所属模块"那条判据（此前从未被任何测试走到过）。
# 必须**单独一段**：上一段的模拟目标与主程序要在这段开始前彻底清理掉。第一版把这段写在
# 上面的 try 里，结果上一段的模拟目标一直活着，注入器反复选中那个"已注入过、管道已断"的
# 旧进程（日志里 10 次重复注入同一个 pid），新目标永远等不到 YZHook —— 断言因此误报失败。
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段八（第二段）：hMod=NULL 的线程内钩子 ===='

$sim8b       = $null
$app8b       = $null
$threadRepl  = $false
$threadAlive = $false

try {
    $sim8b = Start-Process -FilePath $gzyzSim -ArgumentList '--hook-thread' -PassThru
    Start-Sleep -Seconds 4
    $mark8b = Get-LogLength $appLog
    $app8b = Start-Process -FilePath $trainer -PassThru
    Start-Sleep -Seconds 14
    $seg8b       = Read-LogTail $appLog $mark8b
    $threadRepl  = $seg8b -match '已用空过程替换远志键盘/鼠标钩子\(W\)'
    $threadAlive = ($sim8b -and (-not $sim8b.HasExited))
}
finally {
    if ($sim8b -and -not $sim8b.HasExited) { Stop-Process -Id $sim8b.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app8b -and -not $app8b.HasExited) { Stop-Process -Id $app8b.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host ("hMod=NULL 线程内钩子: 替换(W)=" + $threadRepl + " 客户端存活=" + $threadAlive)
if ($threadRepl) { $result.Add('PASS  hMod=NULL 的线程内钩子也被换成空过程（回调地址反查判据有效）') }
else { $result.Add('FAIL  hMod=NULL 的线程内钩子没被处理（回调地址反查判据失效）') }
if ($threadAlive) { $result.Add('PASS  --hook-thread 模式下模拟客户端继续存活') }
else { $result.Add('FAIL  --hook-thread 模式下模拟客户端退出了') }

# ---------------------------------------------------------------------------
# 阶段九：消息层钩子开关（BlockMessageHooks，v0.6.14）
# 目的：把"开关真的能二分"和"开关为 0 时不影响键盘钩子"两条都钉住。
# 后者尤其重要——case 标签不构成作用域，第一版实现把判断写在了键盘那组的标签后面，
# 结果 BlockMessageHooks=0 会把键盘/鼠标钩子一起门控掉（代码里已改成各自分支）。
# 做法：模拟目标（放在路径含 GZYZ 的目录里）延迟 8 秒装两个钩子：键盘 LL + WH_GETMESSAGE。
# 断言：
#   20. BlockMessageHooks=1（默认）：两条"已用空过程替换…"都出现
#   21. BlockMessageHooks=0：键盘那条仍出现，消息层那条**不出现**（客户端照旧存活）
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段九：消息层钩子开关（BlockMessageHooks）===='

$gzyzDir9 = Join-Path $env:TEMP 'GZYZ-yzsim'
$gzyzSim9 = Join-Path $gzyzDir9 'YZSimTarget.exe'

function Invoke-MessageHookPhase([int]$Bit) {
    $ini = @(
        '[General]',
        'Flags=5',
        'WindowPercent=60',
        'LogLevel=3',
        'AutoInject=1',
        'InjectMethod=0',
        'EnableExamGuard=1',
        'ExternalWindowFix=0',
        "BlockMessageHooks=$Bit",
        'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
    )
    Set-Content -LiteralPath $iniFile -Value $ini -Encoding utf8

    $s = $null
    $a = $null
    try {
        $s = Start-Process -FilePath $gzyzSim9 -ArgumentList '--hook', '--hook-message' -PassThru
        Start-Sleep -Seconds 4
        $mark = Get-LogLength $appLog
        $a = Start-Process -FilePath $trainer -PassThru
        Start-Sleep -Seconds 14
        return [pscustomobject]@{
            Seg      = (Read-LogTail $appLog $mark)
            SimAlive = ($s -and (-not $s.HasExited))
        }
    }
    finally {
        if ($s -and -not $s.HasExited) { Stop-Process -Id $s.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 500
        if ($a -and -not $a.HasExited) { Stop-Process -Id $a.Id -Force -ErrorAction SilentlyContinue }
    }
}

New-Item -ItemType Directory -Force -Path $gzyzDir9 | Out-Null
Copy-Item -LiteralPath $sim -Destination $gzyzSim9 -Force

$on  = Invoke-MessageHookPhase 1
$onKey = $on.Seg -match '已用空过程替换远志键盘/鼠标钩子'
$onMsg = $on.Seg -match '已用空过程替换远志消息层钩子'
Write-Host ("BlockMessageHooks=1: 键盘替换=" + $onKey + " 消息层替换=" + $onMsg + " 客户端存活=" + $on.SimAlive)
if ($onKey -and $onMsg) { $result.Add('PASS  BlockMessageHooks=1 时键盘与消息层钩子都被换成空过程') }
else { $result.Add('FAIL  BlockMessageHooks=1 时没有把两类钩子都换掉') }

$off   = Invoke-MessageHookPhase 0
$offKey = $off.Seg -match '已用空过程替换远志键盘/鼠标钩子'
$offMsg = $off.Seg -match '已用空过程替换远志消息层钩子'
Write-Host ("BlockMessageHooks=0: 键盘替换=" + $offKey + " 消息层替换=" + $offMsg + " 客户端存活=" + $off.SimAlive)
if ($offKey -and (-not $offMsg) -and $off.SimAlive) {
    $result.Add('PASS  BlockMessageHooks=0 时只放行消息层、键盘钩子照旧被换掉')
} else {
    $result.Add('FAIL  BlockMessageHooks=0 的行为不对（要么消息层没放行，要么键盘钩子被一起关掉）')
}
# ---------------------------------------------------------------------------
# 阶段十：探针 --watch 的触发式采集（v0.6.15）
# 动机：现场"教师监视时谁在抓屏"有两条自动采集通道——主程序的常驻监控（阶段七已覆盖）
# 与探针 --watch（此前只有手工验证）。探针是随身工具，它坏了现场会整趟白跑，所以钉住：
# 起一个常驻探针 → 让模拟目标加载改名成 ExdDtDup.dll 的替身 → 断言触发落盘与内容。
# 替身与加载器都用与 $DistDir 同位数的一组（x64 用 System32 的 version.dll）。
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段十：探针 --watch 触发式采集 ===='

$probeExe = Join-Path $DistDir 'YZProbe.exe'
if (-not (Test-Path $probeExe)) {
    $result.Add('WARN  找不到 YZProbe.exe，跳过阶段十')
}
else {
    $watchOut = Join-Path $env:TEMP 'yzt-watch-regress'
    New-Item -ItemType Directory -Force -Path $watchOut | Out-Null
    New-Item -ItemType Directory -Force -Path $ddStubDir | Out-Null
    Copy-Item -LiteralPath $ddStubSrc -Destination $ddStub -Force

    $watchProc = $null
    $loadProc  = $null
    try {
        $watchProc = Start-Process -FilePath $probeExe -PassThru -WindowStyle Hidden `
            -ArgumentList '-o', $watchOut, '--watch', '--duration', '26', '--interval', '2', '--no-pause'
        Start-Sleep -Seconds 4
        $loadProc = Start-Process -FilePath $sim -ArgumentList '--load', $ddStub -PassThru
        $watchProc.WaitForExit()
        Start-Sleep -Seconds 1
    }
    finally {
        if ($loadProc -and -not $loadProc.HasExited) { Stop-Process -Id $loadProc.Id -Force -ErrorAction SilentlyContinue }
        if ($watchProc -and -not $watchProc.HasExited) { Stop-Process -Id $watchProc.Id -Force -ErrorAction SilentlyContinue }
    }

    $watchReports = @(Get-ChildItem -LiteralPath $watchOut -Filter 'YZProbe-report-*-watch-capture.md' -ErrorAction SilentlyContinue)
    $watchLogFile = Get-ChildItem -LiteralPath $watchOut -Filter 'YZProbe-watch-*.log' -ErrorAction SilentlyContinue |
                    Sort-Object LastWriteTime | Select-Object -Last 1
    $watchHit = $false
    if ($watchReports.Count -gt 0) {
        $watchHit = (Select-String -LiteralPath $watchReports[0].FullName -Pattern 'ExdDtDup\.dll' -Quiet)
    }
    $logTxt = ''
    if ($watchLogFile) { $logTxt = Get-Content -LiteralPath $watchLogFile.FullName -Raw }
    Write-Host ("watch 报告数=" + $watchReports.Count + " 内容命中=" + $watchHit +
                " 日志有触发行=" + ($logTxt -match '触发落盘\[capture\]'))

    if ($watchReports.Count -gt 0 -and $watchHit) {
        $result.Add('PASS  探针 --watch 触发落盘，且报告里命中加载了 ExdDtDup.dll 的进程')
    } else {
        $result.Add('FAIL  探针 --watch 没有落盘命中采集组件的报告')
    }

    if ($logTxt -match 'watch 结束：共落盘 \d+ 份报告') {
        $result.Add('PASS  探针 --watch 按 --duration 正常退出（日志有结束行，不是崩溃/卡死）')
    } else {
        $result.Add('FAIL  探针 --watch 没有正常退出（watch 日志缺少结束行）')
    }
}
# ---------------------------------------------------------------------------
# 阶段十一：考试/测验强信号熔断（v0.6.16，项目硬边界）
# 为什么单独钉住：AGENTS 写明"考试/测验强信号命中即全功能停用"，而 v0.6.13/v0.6.14 连着两个
# bug 都出在"控制位下发"这条链上（控制位被功能位掩码剔除、ini 读值顺序反了）——考试守护位走的
# 正是同一条链。现有断言里没有任何一条覆盖熔断，所以它一旦失效不会被发现。
# 做法：把模拟目标放进路径含 GZYZ 的目录（强信号 3 要求窗口所属进程在远志路径下）并用 --exam
# 造一个标题含"考试"的可见窗口；主程序正常注入（Flags=5，窗口化打开）。
# 断言：
#   26. EnableExamGuard=1（默认）：日志出现"检测到考试/测验模式"，且广播窗口**没有**被窗口化
#       （GZYZ 目标的状态文件仍是 fullscreen=1 —— 光有日志不够，必须看到功能真的停了）
#   27. EnableExamGuard=0（对照）：同一场景不熔断，窗口化照常生效（fullscreen=0）
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段十一：考试/测验强信号熔断 ===='

$examDir = Join-Path $env:TEMP 'GZYZ-yzsim'
$examSim = Join-Path $examDir 'YZSimTarget.exe'
$examState = Join-Path $examDir 'sim_state.txt'
New-Item -ItemType Directory -Force -Path $examDir | Out-Null
Copy-Item -LiteralPath $sim -Destination $examSim -Force

function Invoke-ExamPhase([int]$Guard) {
    $ini = @(
        '[General]',
        'Flags=5',
        'WindowPercent=60',
        'LogLevel=3',
        'AutoInject=1',
        'InjectMethod=0',
        "EnableExamGuard=$Guard",
        'ExternalWindowFix=0',
        'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
    )
    Set-Content -LiteralPath $iniFile -Value $ini -Encoding utf8

    if (Test-Path -LiteralPath $examState) {
        Move-Item -LiteralPath $examState -Destination "$examState.bak11-$stamp" -Force
    }

    $s = $null
    $a = $null
    try {
        $s = Start-Process -FilePath $examSim -ArgumentList '--exam' -PassThru
        Start-Sleep -Seconds 3
        $mark = Get-LogLength $appLog
        $a = Start-Process -FilePath $trainer -PassThru
        Start-Sleep -Seconds 14
        $state = Get-Content -LiteralPath $examState -ErrorAction SilentlyContinue
        return [pscustomobject]@{
            Seg      = (Read-LogTail $appLog $mark)
            State    = ($state -join ' ')
            SimAlive = ($s -and (-not $s.HasExited))
        }
    }
    finally {
        if ($s -and -not $s.HasExited) { Stop-Process -Id $s.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Milliseconds 500
        if ($a -and -not $a.HasExited) { Stop-Process -Id $a.Id -Force -ErrorAction SilentlyContinue }
    }
}

$examOn = Invoke-ExamPhase 1
$fuseOn = $examOn.Seg -match '检测到考试/测验模式'
$stillFullOn = $examOn.State -match 'fullscreen=1'
Write-Host ("EnableExamGuard=1: 熔断日志=" + $fuseOn + " 仍全屏=" + $stillFullOn + " 客户端存活=" + $examOn.SimAlive)
Write-Host ("  状态: " + $examOn.State)
if ($fuseOn -and $stillFullOn) {
    $result.Add('PASS  考试强信号命中即熔断：日志有熔断记录且窗口确实没有被窗口化')
} else {
    $result.Add('FAIL  考试强信号没有熔断（或熔断后功能仍在生效）——硬边界失效')
}

# 对照组只断言"守护位到了钩子侧并关闭了检测" + "功能恢复"。
# 注意：注入后到配置到达之间（约 1 秒）钩子侧 g_examGuard 仍是 fail-closed 的默认 true，
# 所以这一段里**本来就会有一条**"检测到考试/测验模式"——那是设计行为，不是 bug。
$examOff = Invoke-ExamPhase 0
$guardOff = $examOff.Seg -match '考试模式检测已按配置关闭\(EnableExamGuard=0\)'
$windowizedOff = $examOff.State -match 'fullscreen=0'
Write-Host ("EnableExamGuard=0: 守护位已关闭=" + $guardOff + " 已窗口化=" + $windowizedOff + " 客户端存活=" + $examOff.SimAlive)
Write-Host ("  状态: " + $examOff.State)
if ($guardOff -and $windowizedOff) {
    $result.Add('PASS  对照组 EnableExamGuard=0：守护位确实关闭了检测，功能恢复（窗口化生效）')
} else {
    $result.Add('FAIL  对照组行为不对（考试守护位没有正确下发/生效）')
}
# ---------------------------------------------------------------------------
# 阶段十二：YZSysRun 令牌复制自检（v0.6.17）
# 为什么值得单独钉住：现场部署的**强制启动路径**就是 `YZSysRun.exe -- YZTrainer.exe`
# （学生端以 SYSTEM 运行，主程序必须同级完整性，否则注入、消息钩子、跨进程窗口写全被 UIPI
# 挡下）。这个工具此前一条断言都没有，而它的两种坏法都很致命：
#   ① winlogon 令牌复制失败 → 子进程根本不是 SYSTEM，现场白跑；
#   ② 子进程落到 Session 0 → 探针看不到窗口、挂不上消息队列，数据全部失真。
# 做法：用它起一个 cmd.exe 子进程，并让它把输出写进指定的 txt，然后只匹配 ASCII 片段与数字
# （与输出文件的编码无关）。
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段十二：YZSysRun 令牌复制自检 ===='

$sysrunExe = Join-Path $DistDir 'YZSysRun.exe'
if (-not (Test-Path $sysrunExe)) {
    $result.Add('WARN  找不到 YZSysRun.exe，跳过阶段十二')
}
else {
    $sysrunDir = Join-Path $env:TEMP 'yzt-sysrun-regress'
    New-Item -ItemType Directory -Force -Path $sysrunDir | Out-Null
    $sysrunLog = Join-Path $sysrunDir 'sysrun.txt'
    if (Test-Path -LiteralPath $sysrunLog) { Move-Item -LiteralPath $sysrunLog -Destination "$sysrunLog.bak12-$stamp" -Force }

    try {
        $sr = Start-Process -FilePath $sysrunExe -PassThru -Wait `
            -ArgumentList '--log', $sysrunLog, '--', 'cmd.exe', '/c', 'echo yzt-sysrun-ok'
        Write-Host ("YZSysRun 退出码: " + $sr.ExitCode)
    }
    catch {
        Write-Host ("YZSysRun 启动失败: " + $_.Exception.Message)
    }

    $txt = ''
    if (Test-Path -LiteralPath $sysrunLog) { $txt = Get-Content -LiteralPath $sysrunLog -Raw }
    $mySession = (Get-Process -Id $PID).SessionId

    $tokenOk = $txt -match 'NT AUTHORITY\\SYSTEM[\s\S]{1,80}16384'
    Write-Host ("令牌复制为 SYSTEM: " + $tokenOk)
    if ($tokenOk) { $result.Add('PASS  YZSysRun 复制的令牌是 NT AUTHORITY\SYSTEM（系统完整性）') }
    else { $result.Add('FAIL  YZSysRun 没有自报 SYSTEM 令牌（现场强制启动路径会失效）') }

    $childSession = -1
    $m = [regex]::Match($txt, '已启动[^\r\n]*会话=(\d+)')
    if ($m.Success) { $childSession = [int]$m.Groups[1].Value }
    $childSys = $txt -match '已启动[^\r\n]*16384'
    Write-Host ("子进程会话=" + $childSession + "（当前会话=" + $mySession + "）系统完整性=" + $childSys)
    if ($childSession -eq $mySession -and $childSys) {
        $result.Add('PASS  YZSysRun 的子进程留在当前交互会话且为系统完整性')
    } else {
        $result.Add('FAIL  YZSysRun 的子进程会话或完整性不对（Session 0 会让现场数据全部失真）')
    }
}
# ---------------------------------------------------------------------------
# 阶段十三：免注入路径下的"假全屏"（v0.6.18）
# 阶段三测的是进程内假全屏（AutoInject=1），阶段二测的是跨进程窗口化；而机房"客户端被剥句柄
# 权限、注入不进去"时，唯一的窗口手段就是 ExternalWindowFix 这条跨进程路径——README 明确写了
# 它同样支持假全屏，但此前没有断言。做法与阶段三一致，只是把注入关掉、外部纠正打开。
# 断言：
#   30. 状态文件出现 fullscreen=1 且 topmost=0，且日志有"外部窗口纠正(假全屏)"
#   31. 该窗口没有被加标题栏（外观仍是全屏广播的样子）
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段十三：免注入路径下的假全屏 ===='

foreach ($f in @($stateFile, $capFile)) {
    if (Test-Path $f) { Move-Item -LiteralPath $f -Destination "$f.bak13-$stamp" -Force }
}

$phase13Ini = @(
    '[General]',
    'Flags=36',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=0',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=1',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase13Ini -Encoding utf8

$sim13Proc = $null
$app13Proc = $null
$state13   = ''
$fake13    = $false

try {
    $sim13Proc = Start-Process -FilePath $sim -PassThru
    Start-Sleep -Seconds 3
    $mark13 = Get-LogLength $appLog
    $app13Proc = Start-Process -FilePath $trainer -PassThru

    for ($i = 0; $i -lt 22; $i++) {
        Start-Sleep -Milliseconds 700
        $state13 = Get-Content -LiteralPath $stateFile -ErrorAction SilentlyContinue
        if ($state13 -match 'fullscreen=1' -and $state13 -match 'topmost=0') { $fake13 = $true; break }
    }
    $seg13 = Read-LogTail $appLog $mark13
}
finally {
    if ($sim13Proc -and -not $sim13Proc.HasExited) { Stop-Process -Id $sim13Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app13Proc -and -not $app13Proc.HasExited) { Stop-Process -Id $app13Proc.Id -Force -ErrorAction SilentlyContinue }
}

$log13 = $seg13 -match '外部窗口纠正\(假全屏\)'
Write-Host ("免注入假全屏状态: " + $state13)
Write-Host ("命中 fullscreen=1+topmost=0: " + $fake13 + " ; 日志有外部假全屏记录: " + $log13)
if ($fake13 -and $log13) {
    $result.Add('PASS  免注入路径也能做假全屏（跨进程：保持全屏 + 取消置顶）')
} else {
    $result.Add('FAIL  免注入路径下假全屏没成立（跨进程纠正没做假全屏或没生效）')
}

if ($fake13 -and $state13 -match 'style=0x([0-9A-Fa-f]{8})') {
    $style13 = [Convert]::ToUInt32($Matches[1], 16)
    if (($style13 -band 0x00C00000) -ne 0x00C00000) { $result.Add('PASS  跨进程假全屏下没有加标题栏（外观仍是全屏广播）') }
    else { $result.Add('FAIL  跨进程假全屏下被加了标题栏') }
} else {
    $result.Add('FAIL  免注入假全屏没成立，无法判断标题栏（见上一条）')
}
# ---------------------------------------------------------------------------
# 阶段十四：身份不足时的判读（v0.6.19）
# 本项目历史上最费时间的坑：09-23 把跨进程窗口写的 err=0x5 误判成"内核驱动剥夺句柄权限"，
# 于是"换身份也没用"的结论被写进文档，两个 session 白跑；09-29 用 YZSysRun 换成 SYSTEM 后
# 同一台机器、同一个客户端立刻成功——0x5 其实是 UIPI 的一致性检查。
# 现在本地把两种身份并排复现（目标一律用 YZSysRun 起成 SYSTEM）：
#   阶段 A：主程序用当前的管理员令牌 → 写 SYSTEM 窗口必须被拒，且**不静默**（winfix 记 WARN）
#   阶段 B：主程序也用 YZSysRun 起成 SYSTEM → 同一操作成功
# 断言：
#   32. A 里日志有"外部窗口纠正…失败"，目标仍是 fullscreen=1（确实没改成）
#   33. B 里日志有"外部窗口纠正: 0x… -> …"且目标变成 fullscreen=0
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段十四：身份不足时的判读（UIPI 与 SYSTEM 对照）===='

$identDir   = Join-Path $env:TEMP 'GZYZ-yzsim'
$identSim   = Join-Path $identDir 'YZSimTarget.exe'
$identState = Join-Path $identDir 'sim_state.txt'
$sysrunExe2 = Join-Path $DistDir 'YZSysRun.exe'

if (-not (Test-Path $sysrunExe2)) {
    $result.Add('WARN  找不到 YZSysRun.exe，跳过阶段十四')
}
else {
    New-Item -ItemType Directory -Force -Path $identDir | Out-Null
    Copy-Item -LiteralPath $sim -Destination $identSim -Force

    $identIni = @(
        '[General]',
        'Flags=5',
        'WindowPercent=60',
        'LogLevel=3',
        'AutoInject=0',
        'InjectMethod=0',
        'EnableExamGuard=1',
        'ExternalWindowFix=1',
        'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
    )
    Set-Content -LiteralPath $iniFile -Value $identIni -Encoding utf8

    # 只杀"本轮新增"的目标/主程序进程，避免误伤别的实例
    function Get-NewPids([string]$Name, [datetime]$Since) {
        return @(Get-Process -Name $Name -ErrorAction SilentlyContinue |
                 Where-Object { $_.StartTime -ge $Since } | Select-Object -ExpandProperty Id)
    }
    function Stop-NewPids([string]$Name, [datetime]$Since) {
        foreach ($id in (Get-NewPids $Name $Since)) {
            Stop-Process -Id $id -Force -ErrorAction SilentlyContinue
        }
    }

    # ---- 阶段 A：目标 SYSTEM + 主程序管理员 ----
    $tA = Get-Date
    $srSimA = Start-Process -FilePath $sysrunExe2 -PassThru -WindowStyle Hidden `
        -ArgumentList '--log', (Join-Path $identDir 'sysrun-sim-a.txt'), '--', $identSim
    Start-Sleep -Seconds 4
    if (Test-Path -LiteralPath $identState) { Move-Item -LiteralPath $identState -Destination "$identState.bak14a-$stamp" -Force }
    $mark14a = Get-LogLength $appLog
    $appA = Start-Process -FilePath $trainer -PassThru
    Start-Sleep -Seconds 12
    $seg14a = Read-LogTail $appLog $mark14a
    $stateA = Get-Content -LiteralPath $identState -ErrorAction SilentlyContinue
    Stop-Process -Id $appA.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    Stop-NewPids 'YZSimTarget' $tA
    Stop-Process -Id $srSimA.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800

    $deniedA = $seg14a -match '外部窗口纠正[^\r\n]*失败'
    $fullA   = ($stateA -join ' ') -match 'fullscreen=1'
    Write-Host ("A 身份不足: 记了写失败 WARN=" + $deniedA + " 目标仍全屏=" + $fullA)
    if ($deniedA -and $fullA) {
        $result.Add('PASS  身份不足时跨进程窗口写被 UIPI 拒绝且不静默（记 WARN，窗口仍全屏）')
    } else {
        $result.Add('FAIL  身份不足时的行为不对（要么没记 WARN，要么竟然改成功了）')
    }

    # ---- 阶段 B：目标 SYSTEM + 主程序 SYSTEM ----
    # 注意：YZSysRun 起的**主程序**日志写在 SYSTEM 的 %TEMP%
    # （`C:\Windows\Temp\YZTrainer\yzt.log`），不在学生账户那份里——本轮实测确认。
    # 所以这里两份都看，并且都按"本阶段新增的行"匹配。
    $sysLogPath = Join-Path $env:SystemRoot 'Temp\YZTrainer\yzt.log'
    $tB = Get-Date
    $srSimB = Start-Process -FilePath $sysrunExe2 -PassThru -WindowStyle Hidden `
        -ArgumentList '--log', (Join-Path $identDir 'sysrun-sim-b.txt'), '--', $identSim
    Start-Sleep -Seconds 4
    if (Test-Path -LiteralPath $identState) { Move-Item -LiteralPath $identState -Destination "$identState.bak14b-$stamp" -Force }
    $mark14b    = Get-LogLength $appLog
    $markSysB   = Get-LogLength $sysLogPath
    $srAppB = Start-Process -FilePath $sysrunExe2 -PassThru -WindowStyle Hidden `
        -ArgumentList '--log', (Join-Path $identDir 'sysrun-app-b.txt'), '--', $trainer
    Start-Sleep -Seconds 16
    $seg14b = Read-LogTail $appLog $mark14b
    $segSysB = Read-LogTail $sysLogPath $markSysB
    $stateB = Get-Content -LiteralPath $identState -ErrorAction SilentlyContinue

    $fixedB = ($seg14b -match '外部窗口纠正: 0x') -or ($segSysB -match '外部窗口纠正: 0x')
    $windowizedB = ($stateB -join ' ') -match 'fullscreen=0'
    $wasFullA    = ($stateA -join ' ') -match 'fullscreen=1'
    Write-Host ("B 同级 SYSTEM: 外部纠正成功=" + $fixedB + " 目标已窗口化=" + $windowizedB +
                "（阶段 A 当时仍全屏=" + $wasFullA + "）")
    Write-Host ("  B 目标状态: " + ($stateB -join ' '))
    Write-Host ("  SYSTEM 侧日志: " + $sysLogPath + " 新增 " + ($segSysB.Length) + " 字符")
    if ($fixedB -and $windowizedB -and $wasFullA) {
        $result.Add('PASS  换成 SYSTEM 主程序后跨进程窗口写成功（0x5 是 UIPI，不是驱动剥夺）')
    } else {
        $result.Add('FAIL  换成 SYSTEM 后仍然没成功——与 09-29 判决不符，需要人工复核')
    }

    # 清理：主程序是 SYSTEM，管理员令牌能杀；YZSysRun 自己等到子进程退出才返回
    Stop-NewPids 'YZTrainer' $tB
    Start-Sleep -Milliseconds 500
    Stop-NewPids 'YZSimTarget' $tB
    Start-Sleep -Milliseconds 500
    Stop-Process -Id $srAppB.Id -Force -ErrorAction SilentlyContinue
    Stop-Process -Id $srSimB.Id -Force -ErrorAction SilentlyContinue
}
# ---------------------------------------------------------------------------
# 阶段十五：冻结开启时"别把自己拍进去"（v0.6.20）
# 背景：冻结帧是目标进程里 GetDC(NULL)+BitBlt 抓的整屏 DIB；用户手动勾上防监视的那一刻
# 主界面正好开着，于是主界面被冻进教师会看到的那张图——教师点开"监视/远程桌面"一眼就露馅。
# 两层措施：① 主界面收进托盘（顺序必须是"先隐藏再下发冻结位"，否则第一帧还是脏的）；
#           ② 主窗口设 WDA_EXCLUDEFROMCAPTURE。
# 本机实测（2026-10-11，见 开发记录.md §26）：亲和性对 GDI BitBlt 与 DXGI 桌面复制**都生效**，
# 被排除处露出的是窗口后面的桌面内容（不是黑块）；对照点确认抓屏本身是成功的。
# 断言：
#   34. 冻结期间进程没有可见主窗口（MainWindowHandle=0），日志有那行"已收入托盘"
#   35. 从进程外读回该进程顶层窗口的显示亲和性 = 0x11（WDA_EXCLUDEFROMCAPTURE）
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段十五：冻结开启时把自己从画面里摘出去 ===='

if (-not ('WdaCheck' -as [type])) {
    $wdaSrc = @(
        'using System;',
        'using System.Collections.Generic;',
        'using System.Runtime.InteropServices;',
        'public class WdaCheck {',
        '    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lp);',
        '    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);',
        '    [DllImport("user32.dll")] public static extern bool GetWindowDisplayAffinity(IntPtr h, out uint aff);',
        '    public delegate bool EnumProc(IntPtr h, IntPtr lp);',
        '    public static uint[] AffinitiesOf(uint targetPid) {',
        '        var list = new List<uint>();',
        '        EnumWindows(delegate(IntPtr h, IntPtr lp) {',
        '            uint pid; GetWindowThreadProcessId(h, out pid);',
        '            if (pid == targetPid) {',
        '                uint aff;',
        '                if (GetWindowDisplayAffinity(h, out aff)) list.Add(aff);',
        '                else list.Add(0xFFFFFFFF);',
        '            }',
        '            return true;',
        '        }, IntPtr.Zero);',
        '        return list.ToArray();',
        '    }',
        '}'
    )
    Add-Type -TypeDefinition ($wdaSrc -join "`r`n")
}

$phase15Ini = @(
    '[General]',
    'Flags=13',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=1',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=0',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase15Ini -Encoding utf8

$sim15Proc = $null
$app15Proc = $null

try {
    $sim15Proc = Start-Process -FilePath $sim -PassThru
    Start-Sleep -Seconds 3
    $mark15 = Get-LogLength $appLog
    $app15Proc = Start-Process -FilePath $trainer -PassThru
    Start-Sleep -Seconds 12

    $seg15 = Read-LogTail $appLog $mark15
    $app15Proc.Refresh()
    $mainHwnd  = $app15Proc.MainWindowHandle        # 窗口隐藏后枚举不到可见顶层窗口 → 0
    $hidden    = ($mainHwnd -eq 0) -and (-not $app15Proc.HasExited)
    $logArm    = $seg15 -match '冻结画面：主窗口已设 WDA_EXCLUDEFROMCAPTURE'
    $logFroze  = $seg15 -match '已冻结画面'
    # "亲和性设上了没有"只能由**本进程**自证：2026-10-11 实测 GetWindowDisplayAffinity
    # 跨进程读回来总是 0（设的时候成功、抓屏也确实排除了），所以断言查主程序自己写下的读回值。
    $logAff    = $seg15 -match '主窗口已设 WDA_EXCLUDEFROMCAPTURE（进程内读回=0x11'
    # 进程外枚举只作诊断输出（预期全是 0，见上面的说明，不要把它改成断言）
    $affs      = [WdaCheck]::AffinitiesOf([uint32]$app15Proc.Id)

    Write-Host ("主界面已隐藏=" + $hidden + " 日志有设亲和性=" + $logArm + " 日志有冻结=" + $logFroze +
                " 日志读回0x11=" + $logAff + " 进程外枚举=[" + ($affs -join ',') + "]（预期全 0）")
    if ($hidden -and $logArm) {
        $result.Add('PASS  冻结开启时主界面自动收进托盘（进程无可见主窗口，日志记录已处理）')
    } else {
        $result.Add('FAIL  冻结开启时主界面没有收进托盘（会被冻进教师看到的那张图）')
    }
    if ($logAff -and $logFroze) {
        $result.Add('PASS  主窗口亲和性为 WDA_EXCLUDEFROMCAPTURE（进程内读回 0x11）且冻结已生效')
    } else {
        $result.Add('FAIL  主窗口没设上 WDA_EXCLUDEFROMCAPTURE，或冻结根本没生效')
    }
}
finally {
    if ($sim15Proc -and -not $sim15Proc.HasExited) { Stop-Process -Id $sim15Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app15Proc -and -not $app15Proc.HasExited) { Stop-Process -Id $app15Proc.Id -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# 阶段十六：DXGI 桌面复制取证钩子（v0.6.21）
# 这条钩子只计数、不改变行为（默认关闭，由 TraceDesktopDuplication=1 打开）。它的现场用途：
# 判断"教师监视/广播到底有没有走桌面复制这条链"——尤其是学生机上的 DuplicateOutput 会不会
# 也被"接收广播"用到（若会，盲写 DXGI 冻结就会把学生看广播的画面冻住）。
# 做法：模拟目标放进 GZYZ 目录（当靶子），用 --dup 20 在启动 8 秒后真的建一次桌面复制并取 20 帧；
# 主程序以 TraceDesktopDuplication=1 注入，取证钩子应当接管这两次调用。
# 断言：
#   36. 日志出现"[DXGI 取证] 建立桌面复制成功"（DuplicateOutput 被接管）
#   37. 日志出现"[DXGI 取证] 周期 取帧统计"（AcquireNextFrame 被计数），
#       且模拟目标自己记到"dup: 完成 取到帧=N" 里 N > 0（证明真的取到了帧，不是空转）
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段十六：DXGI 桌面复制取证（只计数）===='

$phase16Ini = @(
    '[General]',
    'Flags=5',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=1',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=0',
    'TraceDesktopDuplication=1',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase16Ini -Encoding utf8

$dupDir   = Join-Path $env:TEMP 'GZYZ-yzsim'
$dupSim   = Join-Path $dupDir 'YZSimTarget.exe'
$dupLog   = Join-Path $dupDir 'sim_log.txt'
New-Item -ItemType Directory -Force -Path $dupDir | Out-Null
Copy-Item -LiteralPath $sim -Destination $dupSim -Force
if (Test-Path -LiteralPath $dupLog) { Move-Item -LiteralPath $dupLog -Destination "$dupLog.bak16-$stamp" -Force }

$sim16Proc = $null
$app16Proc = $null

try {
    $sim16Proc = Start-Process -FilePath $dupSim -ArgumentList '--dup 20' -PassThru
    Start-Sleep -Seconds 4
    $mark16 = Get-LogLength $appLog
    $app16Proc = Start-Process -FilePath $trainer -PassThru
    Start-Sleep -Seconds 14

    $seg16   = Read-LogTail $appLog $mark16
    $traceOk = $seg16 -match '\[DXGI 取证\] 建立桌面复制成功'
    $statOk  = $seg16 -match '\[DXGI 取证\] 周期 取帧统计'
    $simText = if (Test-Path -LiteralPath $dupLog) { Get-Content -LiteralPath $dupLog -Raw } else { '' }
    $doneLine = ([regex]::Match($simText, 'dup: 完成[^\r\n]*')).Value
    $framesOk = $false
    if ($doneLine -match '取到帧=(\d+)') { $framesOk = ([int]$Matches[1] -gt 0) }

    Write-Host ("取证钩子接管:DupOutput=" + $traceOk + " 取帧统计=" + $statOk + " 模拟端: " + $doneLine)
    if ($traceOk) {
        $result.Add('PASS  DXGI 取证钩子接住了 DuplicateOutput（只计数、不改行为）')
    } else {
        $result.Add('FAIL  DXGI 取证钩子没接住 DuplicateOutput（现场就判不了监视是否走这条链）')
    }
    if ($statOk -and $framesOk) {
        $result.Add('PASS  DXGI 取证钩子统计到取帧调用，且模拟端确实取到了帧')
    } else {
        $result.Add('FAIL  DXGI 取帧统计缺失，或模拟端一帧都没取到')
    }
}
finally {
    if ($sim16Proc -and -not $sim16Proc.HasExited) { Stop-Process -Id $sim16Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app16Proc -and -not $app16Proc.HasExited) { Stop-Process -Id $app16Proc.Id -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# 阶段十七：探针的跨架构枚举（v0.6.22）
# 动机：U 盘上带的是 **x64** 的 YZProbe.exe（见 工作顺序.txt【1】），而学生端是 **32 位**进程——
# 64 位进程枚举 WOW64 目标的模块必须带 TH32CS_SNAPMODULE32，漏了这一步不会报错、
# 报告照样生成，只是模块列表为空：现场会以为"没有采集组件"，整趟数据作废。
# 探针的模块枚举（probe.cpp 四处 CreateToolhelp32Snapshot）目前都带了 SNAPMODULE32，
# 但此前从没验证过，所以这里用"32 位模拟目标加载改名成 ExdDtDup.dll 的 32 位替身"钉住：
# 断言 38/39 都只看 **x64** 探针在 32 位目标上的输出。
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段十七：探针跨架构枚举（x64 探针看 32 位目标）===='

$distDir64  = Join-Path (Split-Path -Parent $DistDir) 'x64'
$probeExe64 = Join-Path $distDir64 'YZProbe.exe'

if (-not (Test-Path $probeExe64)) {
    $result.Add('WARN  找不到 dist\x64\YZProbe.exe，跳过阶段十七')
}
else {
    $archOut = Join-Path $env:TEMP 'yzt-probe-arch'
    New-Item -ItemType Directory -Force -Path $archOut | Out-Null
    New-Item -ItemType Directory -Force -Path $ddStubDir | Out-Null
    Copy-Item -LiteralPath $ddStubSrc -Destination $ddStub -Force      # 32 位替身（与 $DistDir 同位数）

    $archSim = $null
    try {
        # 32 位目标要放在**路径含 GZYZ** 的目录里：探针只对"远志路径形状"的进程输出
        # "图形采集栈"行（第一版直接从 dist\Win32\ 起，那行根本不出现，断言 38 误报失败）。
        New-Item -ItemType Directory -Force -Path $gzyzDir | Out-Null
        Copy-Item -LiteralPath $sim -Destination $gzyzSim -Force
        $archSim = Start-Process -FilePath $gzyzSim -ArgumentList '--load', $ddStub -PassThru
        Start-Sleep -Seconds 6
        $archProbe = Start-Process -FilePath $probeExe64 -PassThru -WindowStyle Hidden `
            -ArgumentList '-o', $archOut, '--no-pause'
        $archProbe.WaitForExit()
        Start-Sleep -Seconds 1

        $archReport = Get-ChildItem -LiteralPath $archOut -Filter 'YZProbe-report-*.md' -ErrorAction SilentlyContinue |
                      Sort-Object LastWriteTime | Select-Object -Last 1
        $archTxt = ''
        if ($archReport) { $archTxt = Get-Content -LiteralPath $archReport.FullName -Raw }

        # 38：该 pid 的"图形采集栈"行必须命中 ExdDtDup.dll，且判词落到 DXGI
        $stackLine  = ([regex]::Match($archTxt, "图形采集栈 PID=$($archSim.Id) [^\r\n]*")).Value
        $stackOk    = ($stackLine -match 'ExdDtDup\.dll=是') -and ($stackLine -match '采集层=DXGI')
        # 39：全进程扫描那条口径也要带上该 pid（另一处枚举代码）
        $scanLine   = ([regex]::Match($archTxt, "采集组件扫描命中：PID=$($archSim.Id) [^\r\n]*")).Value
        $scanOk     = ($scanLine -match 'ExdDtDup\.dll')

        Write-Host ("x64 探针报告=" + $(if ($archReport) { $archReport.Name } else { '(无)' }))
        Write-Host ("  图形采集栈行: " + $stackLine)
        Write-Host ("  扫描命中行: " + $scanLine)
        if ($stackOk) {
            $result.Add('PASS  x64 探针能枚举 32 位目标的模块并判定采集层=DXGI（SNAPMODULE32 有效）')
        } else {
            $result.Add('FAIL  x64 探针看不到 32 位目标的模块（现场会误判"没有采集组件"，整趟白跑）')
        }
        if ($scanOk) {
            $result.Add('PASS  x64 探针的全进程采集组件扫描也命中该 32 位目标')
        } else {
            $result.Add('FAIL  x64 探针的全进程扫描漏掉了 32 位目标')
        }
    }
    finally {
        if ($archSim -and -not $archSim.HasExited) { Stop-Process -Id $archSim.Id -Force -ErrorAction SilentlyContinue }
    }
}

# ---------------------------------------------------------------------------
# 阶段十八：探针不能把"仅因采集组件晋级的进程"报成受 PPL 保护（v0.6.23 修的 bug）
# 根因：保护级别原来只在 p.isYuanzhi 为真时查，而"采集组件轻量扫描"（v0.6.12 加的）会在之后
# 把进程晋级成远志——那类进程正是现场最想看的"真正持 DuplicateOutput 的那个"。
# 于是级别留在初值 0，而映射表里 0 = "PPL/WinTcbLight"：现场会读成"这个抓屏进程受 PPL 保护"。
# 做法：把模拟目标放在**不带远志路径特征**的目录里、让它加载改名成 ExdDtDup.dll 的替身
# （于是它只能靠"采集组件扫描"晋级），再用 **x64** 探针扫一遍，检查那一行的"进程保护"列。
# 断言：
#   40. 它确实出现在"远志相关进程"表里（晋级口径仍在——否则这条断言失去意义）
#   41. 它的"进程保护"列不是 PPL/WinTcbLight，而是"无"/"无保护"（修好的那条）
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段十八：探针的保护级别判读（仅因采集组件晋级的进程）===='

$nonVendorDir = Join-Path $env:TEMP 'yzt-not-yuanzhi'
$nonVendorSim = Join-Path $nonVendorDir 'YZSimTarget.exe'
New-Item -ItemType Directory -Force -Path $nonVendorDir | Out-Null
New-Item -ItemType Directory -Force -Path $ddStubDir | Out-Null
Copy-Item -LiteralPath $sim -Destination $nonVendorSim -Force
Copy-Item -LiteralPath $ddStubSrc -Destination $ddStub -Force

$protOut = Join-Path $env:TEMP 'yzt-probe-prot'
New-Item -ItemType Directory -Force -Path $protOut | Out-Null

$protSim = $null
try {
    $protSim = Start-Process -FilePath $nonVendorSim -ArgumentList '--load', $ddStub -PassThru
    Start-Sleep -Seconds 6
    $protProbe = Start-Process -FilePath $probeExe64 -PassThru -WindowStyle Hidden `
        -ArgumentList '-o', $protOut, '--no-pause'
    $protProbe.WaitForExit()
    Start-Sleep -Seconds 1

    $protReport = Get-ChildItem -LiteralPath $protOut -Filter 'YZProbe-report-*.md' -ErrorAction SilentlyContinue |
                  Sort-Object LastWriteTime | Select-Object -Last 1
    $protTxt = ''
    if ($protReport) { $protTxt = Get-Content -LiteralPath $protReport.FullName -Raw }
    $protRow = ([regex]::Match($protTxt, "\| $($protSim.Id) \| [^\r\n]*")).Value

    $inTable   = ($protRow -ne '')
    $badLabel  = ($protRow -match 'PPL/WinTcbLight')
    $goodLabel = ($protRow -match '\| 无 \|') -or ($protRow -match '无保护')

    Write-Host ("该 pid 在远志相关进程表里=" + $inTable + " 行: " + $protRow)
    if ($inTable) {
        $result.Add('PASS  仅因采集组件晋级的进程确实进了"远志相关进程"表（晋级口径仍在）')
    } else {
        $result.Add('FAIL  仅因采集组件晋级的进程没进表——现场会漏掉真正抓屏的那个进程')
    }
    if ($inTable -and -not $badLabel -and $goodLabel) {
        $result.Add('PASS  该进程的保护级别判读正确（无保护），没有被误报成 PPL/WinTcbLight')
    } else {
        $result.Add('FAIL  该进程的保护级别判读错误（现场会误判"抓屏进程受 PPL 保护"）')
    }
}
finally {
    if ($protSim -and -not $protSim.HasExited) { Stop-Process -Id $protSim.Id -Force -ErrorAction SilentlyContinue }
}

# ---------------------------------------------------------------------------
# 阶段十九：x64 探针的 --watch 在 32 位目标上的跨架构验证（v0.6.24）
# 为什么单独一条：现场采"教师监视时谁在抓屏"的主力通道就是 **x64 探针的 --watch**
# （一次性扫描在阶段十七已覆盖跨架构，但 --watch 是另一条代码路径：常驻轮询 + 触发判定 +
# 报告落盘，见 probe.cpp 的 RunWatch）。它要是漏了 32 位目标的模块，现场照样白跑。
# 做法与阶段十七同源：32 位模拟目标放 GZYZ 目录、加载改名成 ExdDtDup.dll 的替身；
# 先挂 **x64** 探针 --watch，4 秒后再起目标，让它触发落盘。
# 断言：
#   42. 出现 -watch-capture 报告，且报告里含该 pid 的 ExdDtDup 命中
#   43. 那份报告里该 pid 的"图形采集栈"行判为 DXGI（跨架构下 stack 判词也成立）
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段十九：x64 探针 --watch 跨架构（32 位目标）===='

if (-not (Test-Path $probeExe64)) {
    $result.Add('WARN  找不到 dist\x64\YZProbe.exe，跳过阶段十九')
}
else {
    $watchOut64 = Join-Path $env:TEMP 'yzt-watch-arch'
    New-Item -ItemType Directory -Force -Path $watchOut64 | Out-Null
    New-Item -ItemType Directory -Force -Path $gzyzDir | Out-Null
    Copy-Item -LiteralPath $sim -Destination $gzyzSim -Force
    New-Item -ItemType Directory -Force -Path $ddStubDir | Out-Null
    Copy-Item -LiteralPath $ddStubSrc -Destination $ddStub -Force

    $watch64 = $null
    $load64  = $null
    try {
        $watch64 = Start-Process -FilePath $probeExe64 -PassThru -WindowStyle Hidden `
            -ArgumentList '-o', $watchOut64, '--watch', '--duration', '22', '--interval', '2', '--no-pause'
        Start-Sleep -Seconds 4
        $load64 = Start-Process -FilePath $gzyzSim -ArgumentList '--load', $ddStub -PassThru
        $watch64.WaitForExit()
        Start-Sleep -Seconds 1
    }
    finally {
        if ($load64  -and -not $load64.HasExited)  { Stop-Process -Id $load64.Id  -Force -ErrorAction SilentlyContinue }
        if ($watch64 -and -not $watch64.HasExited) { Stop-Process -Id $watch64.Id -Force -ErrorAction SilentlyContinue }
    }

    $rep64 = Get-ChildItem -LiteralPath $watchOut64 -Filter 'YZProbe-report-*-watch-capture.md' -ErrorAction SilentlyContinue |
             Sort-Object LastWriteTime | Select-Object -Last 1
    $txt64 = ''
    if ($rep64) { $txt64 = Get-Content -LiteralPath $rep64.FullName -Raw }
    $pidHit  = ($txt64 -match "PID=$($load64.Id)") -and ($txt64 -match 'ExdDtDup')
    $stack64 = ([regex]::Match($txt64, "图形采集栈 PID=$($load64.Id) [^\r\n]*")).Value
    $stackOk = ($stack64 -match '采集层=DXGI')

    Write-Host ("x64 --watch 报告=" + $(if ($rep64) { $rep64.Name } else { '(无)' }) +
                " 命中该 pid=" + $pidHit)
    Write-Host ("  图形采集栈行: " + $stack64)
    if ($pidHit) {
        $result.Add('PASS  x64 探针 --watch 在 32 位目标上触发落盘并命中采集组件')
    } else {
        $result.Add('FAIL  x64 探针 --watch 没在 32 位目标上触发（现场主力采集通道会白跑）')
    }
    if ($stackOk) {
        $result.Add('PASS  x64 --watch 报告里的跨架构采集栈判词正确（采集层=DXGI）')
    } else {
        $result.Add('FAIL  x64 --watch 报告里缺少正确的图形采集栈判词')
    }
}

# ---------------------------------------------------------------------------
# 阶段二十：InjectMethod=1（消息钩子注入）这条配置路径（v0.6.25）
# 为什么补：README 把它写成"远程线程注入失败时的兜底办法"（第 121 行的现场指引就是
# "把 InjectMethod 改成 1 再试"），但我们此前只测过默认的 0（远程线程）与**运行期自动回退**，
# 从没测过"ini 里直接指定 1"。这条路径若坏了，现场按 README 操作会直接卡死。
# 注意它的固有副作用（README 第 338 行已写明）：WH_GETMESSAGE + dwThreadId=0 是**会话级**全局钩子，
# 我们的 DLL 会被映射进本会话所有会取消息的 GUI 进程；非目标进程里引擎会自检后放弃
# （日志"宿主进程不是远志学生端/模拟目标，Hook 不启用"），所以断言只认目标进程那条线。
# 断言：
#   44. 日志出现"已通过消息钩子注入 HHOOK=…"（走的是消息钩子路径，且拿到合法句柄）
#   45. 窗口化照常生效（模拟目标状态 fullscreen=0）——说明引擎真的在目标进程里跑起来了
#   46. 该阶段日志段里**没有**"已注入目标进程 pid="（injectMethod=1 时不应再尝试远程线程注入）
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段二十：InjectMethod=1 消息钩子注入 ===='

foreach ($f in @($stateFile, $capFile)) {
    if (Test-Path $f) { Move-Item -LiteralPath $f -Destination "$f.bak20-$stamp" -Force }
}

$phase20Ini = @(
    '[General]',
    'Flags=5',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=1',
    'InjectMethod=1',
    'EnableExamGuard=1',
    'ExternalWindowFix=0',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase20Ini -Encoding utf8

$sim20Proc = $null
$app20Proc = $null
$state20   = ''

try {
    $sim20Proc = Start-Process -FilePath $sim -PassThru
    Start-Sleep -Seconds 3
    $mark20 = Get-LogLength $appLog
    $app20Proc = Start-Process -FilePath $trainer -PassThru

    # 消息钩子要等目标线程取一次消息才会把 DLL 映射进去，这里多等一会儿
    for ($i = 0; $i -lt 24; $i++) {
        Start-Sleep -Milliseconds 700
        $state20 = Get-Content -LiteralPath $stateFile -ErrorAction SilentlyContinue
        if (($state20 -join ' ') -match 'fullscreen=0') { break }
    }
    $seg20 = Read-LogTail $appLog $mark20
}
finally {
    if ($sim20Proc -and -not $sim20Proc.HasExited) { Stop-Process -Id $sim20Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app20Proc -and -not $app20Proc.HasExited) { Stop-Process -Id $app20Proc.Id -Force -ErrorAction SilentlyContinue }
}

$hookInj   = $seg20 -match '已通过消息钩子注入 HHOOK=(?:0x)?0*[1-9A-Fa-f]'
$windowized20 = ($state20 -join ' ') -match 'fullscreen=0'
$noRemote  = -not ($seg20 -match '已注入目标进程 pid=')

Write-Host ("消息钩子注入=" + $hookInj + " 已窗口化=" + $windowized20 + " 未走远程线程=" + $noRemote)
Write-Host ("  状态: " + ($state20 -join ' '))
if ($hookInj -and $windowized20) {
    $result.Add('PASS  InjectMethod=1 时消息钩子注入成功，且引擎在目标进程里真的生效（窗口化）')
} else {
    $result.Add('FAIL  InjectMethod=1 的消息钩子注入没走通或没生效（现场按 README 兜底会卡死）')
}
if ($noRemote) {
    $result.Add('PASS  InjectMethod=1 时没有再去尝试远程线程注入（配置被正确尊重）')
} else {
    $result.Add('FAIL  InjectMethod=1 时仍然尝试了远程线程注入（配置没被尊重）')
}

# ---------------------------------------------------------------------------
# 阶段二十一：界面/热键那条开关链（v0.6.26）
# 为什么补：CmdSetFlag 对四个功能位（窗口化/键鼠解锁/防监视/拦遥控）是**只发 YZ_CMD_SET_* opcode、
# 不发整份配置**（ui.cpp：if (opcode != 0) IpcSendFlagCommand(...) else IpcSendConfig()），
# 也就是用户勾复选框、按热键时走的是钩子侧 EngineSetFlag 那条链；回归此前只写 ini，从没碰过它。
#
# 本轮踩到的两件事（都记进 开发记录 §33）：
#   ① 本机 Ctrl+Alt+F9 已被别的程序占用，RegisterHotKey 直接失败（err=1409），而老代码静默——
#      现在注册失败会记 WARN（产品改动）。
#   ② 用 keybd_event 合成组合键在"只有主程序时"能触发热键（实测 ini Flags 5→13），
#      但在"模拟目标在场"时没能触发——属测试环境的输入注入问题，不值得让回归依赖它。
#      所以这里改成**确定性驱动**：复选框走 WM_COMMAND + BM_SETCHECK（与鼠标点击同一条代码路径），
#      热键走直接投递 WM_HOTKEY（与系统投递同一个处理器）。链路覆盖不变，且不依赖输入队列。
# 断言：
#   47. 勾上"防监视"复选框（WM_COMMAND）后：ini 出现防监视位、且抓帧哈希停止变化
#   48. 再取消勾选后：抓帧哈希又开始变化（这条链两个方向都通）
#   49. WM_HOTKEY(F12) 真的能隐藏主界面、再发一次又恢复显示
#      —— 钉住 v0.6.26 修的"F12 原来只显示、不隐藏"那个 bug
# ---------------------------------------------------------------------------
Write-Host ''
Write-Host '==== 阶段二十一：界面/热键开关链（YZ_CMD_SET_*）===='

if (-not ('UiDrive' -as [type])) {
    $uiSrc = @(
        'using System;',
        'using System.Runtime.InteropServices;',
        'public class UiDrive {',
        '    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);',
        '    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);',
        '    const uint BM_SETCHECK = 0x00F1;',
        '    const uint WM_COMMAND  = 0x0111;',
        '    const uint WM_HOTKEY   = 0x0312;',
        '    const int  BST_UNCHECKED = 0;',
        '    const int  BST_CHECKED   = 1;',
        '    /* 与鼠标点击同一条代码路径：先把复选框设成目标状态，再发 WM_COMMAND 通知 */',
        '    public static bool ClickCheckbox(IntPtr main, int ctrlId, bool wantChecked) {',
        '        IntPtr ctl = GetDlgItem(main, ctrlId);',
        '        if (ctl == IntPtr.Zero) return false;',
        '        SendMessage(ctl, BM_SETCHECK, (IntPtr)(wantChecked ? BST_CHECKED : BST_UNCHECKED), IntPtr.Zero);',
        '        SendMessage(main, WM_COMMAND, (IntPtr)ctrlId, ctl);',
        '        return true;',
        '    }',
        '    public static void PostHotkey(IntPtr main, int hotkeyId) {',
        '        SendMessage(main, WM_HOTKEY, (IntPtr)hotkeyId, IntPtr.Zero);',
        '    }',
        '}'
    )
    Add-Type -TypeDefinition ($uiSrc -join "`r`n")
}

function Get-CaptureHashes([int]$Take) {
    if (-not (Test-Path -LiteralPath $capFile)) { return @() }
    $l = Get-Content -LiteralPath $capFile | Select-Object -Last $Take
    return @($l | ForEach-Object { ($_ -split ',')[1] } | Where-Object { $_ })
}
function Get-IniFlags {
    $m = (Get-Content -LiteralPath $iniFile | Select-String '^Flags=')
    if (-not $m) { return -1 }
    return [int](($m -join '') -replace '^Flags=', '')
}

$IDC_CHK_ANTIMON = 2004
$ID_HOTKEY_SHOWUI = 0x7104

foreach ($f in @($stateFile, $capFile)) {
    if (Test-Path $f) { Move-Item -LiteralPath $f -Destination "$f.bak21-$stamp" -Force }
}

$phase21Ini = @(
    '[General]',
    'Flags=5',
    'WindowPercent=60',
    'LogLevel=3',
    'AutoInject=1',
    'InjectMethod=0',
    'EnableExamGuard=1',
    'ExternalWindowFix=0',
    'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe;YZSimTarget.exe'
)
Set-Content -LiteralPath $iniFile -Value $phase21Ini -Encoding utf8

$sim21Proc = $null
$app21Proc = $null
$state21   = ''
$liveBefore = $false
$frozenOn   = $false
$frozenOff  = $false
$flagsOn    = -1
$hwndVisible = -1
$hwndHidden  = -1
$hwndBack    = -1
$clickOk     = $false

try {
    $sim21Proc = Start-Process -FilePath $sim -PassThru
    Start-Sleep -Seconds 3
    $app21Proc = Start-Process -FilePath $trainer -PassThru

    for ($i = 0; $i -lt 20; $i++) {
        Start-Sleep -Milliseconds 700
        $state21 = Get-Content -LiteralPath $stateFile -ErrorAction SilentlyContinue
        if (($state21 -join ' ') -match 'fullscreen=0') { break }
    }

    # 基线：防监视没开时，哈希应当在变
    Start-Sleep -Seconds 3
    $h0 = Get-CaptureHashes 6
    $liveBefore = (@($h0 | Select-Object -Unique).Count -gt 1)

    # ① 勾上"防监视"复选框（界面路径 → YZ_CMD_SET_ANTI_MONITOR）
    $app21Proc.Refresh()
    $mainHwnd21 = $app21Proc.MainWindowHandle
    $clickOk = [UiDrive]::ClickCheckbox([IntPtr]$mainHwnd21, $IDC_CHK_ANTIMON, $true)
    Start-Sleep -Seconds 4
    $flagsOn = Get-IniFlags
    $h1 = Get-CaptureHashes 6
    $frozenOn = ($h1.Count -ge 3) -and (@($h1 | Select-Object -Unique).Count -eq 1) -and (($flagsOn -band 8) -ne 0)

    # ② 取消勾选
    [void][UiDrive]::ClickCheckbox([IntPtr]$mainHwnd21, $IDC_CHK_ANTIMON, $false)
    Start-Sleep -Seconds 4
    $h2 = Get-CaptureHashes 6
    $frozenOff = (@($h2 | Select-Object -Unique).Count -gt 1)

    # ③ WM_HOTKEY(F12)：隐藏 → 再发一次恢复显示
    $app21Proc.Refresh()
    $hwndVisible = $app21Proc.MainWindowHandle
    [UiDrive]::PostHotkey([IntPtr]$mainHwnd21, $ID_HOTKEY_SHOWUI)
    Start-Sleep -Seconds 2
    $app21Proc.Refresh()
    $hwndHidden = $app21Proc.MainWindowHandle
    [UiDrive]::PostHotkey([IntPtr]$mainHwnd21, $ID_HOTKEY_SHOWUI)
    Start-Sleep -Seconds 2
    $app21Proc.Refresh()
    $hwndBack = $app21Proc.MainWindowHandle
}
finally {
    if ($sim21Proc -and -not $sim21Proc.HasExited) { Stop-Process -Id $sim21Proc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if ($app21Proc -and -not $app21Proc.HasExited) { Stop-Process -Id $app21Proc.Id -Force -ErrorAction SilentlyContinue }
}

Write-Host ("复选框命中=" + $clickOk + " 基线实时=" + $liveBefore + " 勾选后冻结=" + $frozenOn +
            "（ini Flags=" + $flagsOn + "）取消后恢复实时=" + $frozenOff)
Write-Host ("WM_HOTKEY(F12): 初始句柄=" + $hwndVisible + " 隐藏后=" + $hwndHidden + " 再发后=" + $hwndBack)

if ($frozenOn) {
    $result.Add('PASS  界面复选框（WM_COMMAND）能打开防监视：ini 出现防监视位且抓帧哈希停止变化')
} else {
    $result.Add('FAIL  界面复选框没能打开防监视（YZ_CMD_SET_* 那条链没生效）')
}
if ($frozenOff) {
    $result.Add('PASS  取消勾选后恢复实时：抓帧哈希又开始变化（这条链两个方向都通）')
} else {
    $result.Add('FAIL  取消勾选后画面没有恢复实时')
}
if ($hwndVisible -ne 0 -and $hwndHidden -eq 0 -and $hwndBack -ne 0) {
    $result.Add('PASS  Ctrl+Alt+F12 的处理器能显示/隐藏主界面（v0.6.26 修的"只显示不隐藏"）')
} else {
    $result.Add('FAIL  F12 的显示/隐藏没生效（或只有单向）')
}

Write-Host '==== 测试结果 ===='
$result | ForEach-Object { Write-Host $_ }
$failCount = @($result | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host ''
Write-Host "失败项: $failCount"
if ($failCount -gt 0) { exit 1 }
exit 0
