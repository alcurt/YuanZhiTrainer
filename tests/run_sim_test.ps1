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
function Get-LogLength([string]$Path) {
    if ([string]::IsNullOrEmpty($Path)) { return 0 }
    if (Test-Path -LiteralPath $Path) { return (Get-Item -LiteralPath $Path).Length }
    return 0
}
function Read-LogTail([string]$Path, [long]$From) {
    if ([string]::IsNullOrEmpty($Path)) { return '' }
    if (-not (Test-Path -LiteralPath $Path)) { return '' }
    $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
                                 [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        [void]$fs.Seek($From, [System.IO.SeekOrigin]::Begin)
        $sr = New-Object System.IO.StreamReader($fs)
        return $sr.ReadToEnd()
    }
    finally { $fs.Dispose() }
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
Write-Host '==== 测试结果 ===='
$result | ForEach-Object { Write-Host $_ }
$failCount = @($result | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host ''
Write-Host "失败项: $failCount"
if ($failCount -gt 0) { exit 1 }
exit 0
