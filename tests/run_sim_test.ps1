<#
    YZTrainer 本地回归测试（不需要机房）：
    用 YZSimTarget 模拟远志学生端的全屏广播、键鼠封锁与定期抢回全屏的行为。

    请以管理员身份运行（YZTrainer.exe 需要提权；自动测试时不希望弹 UAC）。

    阶段一（注入路径，ExternalWindowFix=0，只看进程内 Hook）断言 4 条：
      1. 模拟目标启动后处于全屏置顶状态
      2. YZTrainer 启动后模拟窗口被窗口化（不再是全屏）
      3. 窗口样式已包含 WS_CAPTION（真的变成普通窗口）
      4. 开启防监视后，模拟端连续抓帧哈希保持不变（画面被冻结）

    阶段二（免注入兜底，AutoInject=0 + ExternalWindowFix=1）断言 2 条 + 1 条提示：
      5. 完全不注入的情况下，模拟窗口仍被主程序跨进程改成普通窗口
      6. 外部纠正后的窗口样式包含 WS_CAPTION
      7. 日志里能看到“外部窗口纠正”记录（找不到只记 WARN，不影响结论）

    阶段三（假全屏，Flags=4|32）断言 2 条：
      8. 广播窗口仍铺满整屏（fullscreen=1）且没有标题栏
      9. 广播窗口已不置顶（topmost=0）——这是"能切到自己的窗口"的前提

    阶段四（远程执行审计，YZSimTarget --exec）断言 2 条：
     10. remote-exec.log 里出现 CreateProcessW 记录
     11. 该记录被标为 origin=remote（不是客户端自己拉起的辅助进程）

    阶段五（主动拔钩的晚加载重试，v0.6.3）断言 1 条：
     12. 钩子模块在注入之后才被加载时，主程序会重试主动拔钩
         （日志出现"NativeUnhook: 目标模块 ExdHooks.dll 已加载，开始尝试"）

    阶段六（反横跳闸门，v0.6.6）断言 1 条：
     13. 客户端每 200ms 就把无边框全屏抢回来（YZSimTarget --fight）时，
         外部纠正必须**认输停手**，而不是陪着一起高频横跳
         （日志出现"已对该窗口停手"）

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

try {
    Write-Host '启动模拟目标 ...'
    $simProc = Start-Process -FilePath $sim -PassThru
    Start-Sleep -Seconds 3

    $state1 = Get-Content -LiteralPath $stateFile -ErrorAction SilentlyContinue
    Write-Host "模拟目标状态: $state1"
    if ($state1 -match 'fullscreen=1') { $result.Add('PASS  模拟目标已进入全屏置顶') }
    else { $result.Add('FAIL  模拟目标未进入全屏') }

    Write-Host '启动 YZTrainer ...'
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
    if ((Test-Path $appLog) -and (Select-String -LiteralPath $appLog -Pattern '外部窗口纠正' -Quiet)) {
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
        $app5Proc = Start-Process -FilePath $trainer -PassThru
        Start-Sleep -Seconds 14

        $retryHit = $false
        $skipHit  = $false
        if (Test-Path $appLog) {
            $retryHit = (Select-String -LiteralPath $appLog -Pattern 'NativeUnhook: 目标模块 ExdHooks\.dll 已加载，开始尝试' -Quiet)
            $skipHit  = (Select-String -LiteralPath $appLog -Pattern '找不到导出 UnSetExdHooks' -Quiet)
        }
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
    $app6Proc = Start-Process -FilePath $trainer -PassThru
    Start-Sleep -Seconds 12

    $gaveUp = (Test-Path $appLog) -and
              (Select-String -LiteralPath $appLog -Pattern '已对该窗口停手' -Quiet)
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
Write-Host '==== 测试结果 ===='
$result | ForEach-Object { Write-Host $_ }
$failCount = @($result | Where-Object { $_ -like 'FAIL*' }).Count
Write-Host ''
Write-Host "失败项: $failCount"
if ($failCount -gt 0) { exit 1 }
exit 0
