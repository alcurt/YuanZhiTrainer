# YZTrainer 机房数据判读（v0.6.40 起随现场清单使用）
#
# 目的：把"回来以后手工翻日志"变成一条命令。输入是 tools\collect-field-data.ps1 收回来的目录或 zip，
#       输出一张判读表：客户端连接数、抓屏子进程识别、冻结武装、补充通道、取证统计、
#       考试熔断、UIPI 失败、告警汇总、以及键鼠探针 A/B/C/D 的结论。
#
# 用法（5.1 或 pwsh 7 均可）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\analyze-field-data.ps1 -Path D:\带回\field-20261015-101500
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\analyze-field-data.ps1 -Path D:\带回\field-xxx.zip
# 不传 -Path 时：在当前目录里找最新的 field-* 目录/zip。
#
# 说明：判读**只做统计与摘录**，不替你做结论；"冻结是否真的生效"最终以教师端画面/缩略图是否还在动为准。

param(
    [string]$Path = '',
    [string]$OutFile = ''
)

$ErrorActionPreference = 'Continue'
$work = $Path
$tempExpanded = $null

if ([string]::IsNullOrEmpty($work)) {
    $cand = Get-ChildItem -Directory -Filter 'field-*' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
    if ($cand) { $work = $cand.FullName }
    else {
        $z = Get-ChildItem -File -Filter 'field-*.zip' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime | Select-Object -Last 1
        if ($z) { $work = $z.FullName }
    }
}
if ([string]::IsNullOrEmpty($work) -or -not (Test-Path -LiteralPath $work -ErrorAction SilentlyContinue)) {
    Write-Host '找不到待判读的数据：请用 -Path 指定 collect-field-data.ps1 收回来的目录或 zip。' -ForegroundColor Yellow
    exit 2
}

if ((Get-Item -LiteralPath $work).PSIsContainer -eq $false -and $work -like '*.zip') {
    $tempExpanded = Join-Path $env:TEMP ("field-unzip-{0}" -f (Get-Date -Format 'HHmmss'))
    New-Item -ItemType Directory -Path $tempExpanded -Force | Out-Null
    try {
        Expand-Archive -LiteralPath $work -DestinationPath $tempExpanded -Force
        $work = $tempExpanded
    } catch {
        Write-Host ('解压失败：' + $_.Exception.Message) -ForegroundColor Red
        exit 2
    }
}

$log = New-Object System.Collections.Generic.List[string]
function L([string]$s) { $log.Add($s); Write-Host $s }

L ("==== YZTrainer 机房数据判读 {0} ====" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
L ("数据目录: " + $work)
L ''

# 汇总所有 yzt*.log（含轮转档）与探针报告
$logs = @(Get-ChildItem -LiteralPath $work -Recurse -File -Filter 'yzt*.log' -ErrorAction SilentlyContinue)
$probes = @(Get-ChildItem -LiteralPath $work -Recurse -File -Filter 'input-probe-*.txt' -ErrorAction SilentlyContinue)
$diagDirs = @(Get-ChildItem -LiteralPath $work -Recurse -Directory -Filter 'diag-*' -ErrorAction SilentlyContinue)

L '---- 0. 收到的东西 ----'
L ("  yzt*.log 文件: {0} 个" -f $logs.Count)
foreach ($f in $logs) { L ("    {0}  ({1} 字节)" -f $f.FullName.Substring($work.Length).TrimStart('\'), $f.Length) }
L ("  诊断包 diag-*: {0} 个" -f $diagDirs.Count)
foreach ($d in $diagDirs) { L ("    {0}  ({1} 个文件)" -f $d.Name, (Get-ChildItem $d.FullName -Recurse -File -EA SilentlyContinue).Count) }
L ("  键鼠探针报告: {0} 个" -f $probes.Count)
L ''

if ($logs.Count -eq 0) {
    L '没找到 yzt*.log —— 数据不完整（现场是否没起主程序？或回收目录选错了？）。'
} else {
    $text = ''
    foreach ($f in $logs) { $text += (Get-Content -LiteralPath $f.FullName -Raw -Encoding UTF8) + "`r`n" }

    function Count-Of([string]$pattern) { return @([regex]::Matches($text, $pattern)).Count }
    function Last-Of([string]$pattern) {
        $m = [regex]::Matches($text, $pattern)
        if ($m.Count -eq 0) { return '' }
        return $m[$m.Count - 1].Value
    }

    L '---- 1. 管道客户端（v0.6.38 起期望 >= 2：学生端 + 抓屏子进程）----'
    $connects = [regex]::Matches($text, 'Hook DLL 已连接: pid=(\d+)')
    $pids = @()
    foreach ($m in $connects) { $pids += $m.Groups[1].Value }
    L ("  连接事件 {0} 次；pid: {1}" -f $connects.Count, (($pids | Select-Object -Unique) -join ', '))
    if ($connects.Count -ge 2) { L '  => 符合多客户端预期' } else { L '  => 只看到 1 个或 0 个客户端：抓屏子进程可能没被识别/没连上' }

    L ''
    L '---- 2. 抓屏子进程识别与注入 ----'
    L ("  抓屏子进程出现: {0} 次" -f (Count-Of '抓屏子进程出现[:：]\s*pid='))
    L ("  按 PID 注入结论行: {0} 次" -f (Count-Of '抓屏子进程 pid=\d+[:：]\s*已注入'))
    L ("    最后一条: " + (Last-Of '抓屏子进程 pid=\d+[:：]\s*[^\r\n]*'))

    L ''
    L '---- 3. 冻结是否被武装 ----'
    L ("  DXGI 冻结: 已开启   {0} 次" -f (Count-Of 'DXGI 冻结: 已开启'))
    L ("  DXGI 冻结: 已关闭   {0} 次" -f (Count-Of 'DXGI 冻结: 已关闭'))
    L ("  [DXGI 判定] 行      {0} 次" -f (Count-Of '\[DXGI 判定\]'))
    $lastJudge = Last-Of '\[DXGI 判定\][^\r\n]*'
    if ($lastJudge) { L ("    最后一次判定: " + $lastJudge) }
    $lastStat = Last-Of '\[DXGI 取证\] 取帧统计[^\r\n]*'
    if ($lastStat) { L ("    最后一次取证: " + $lastStat) }

    L ''
    L '---- 4. 补充状态通道（抓屏子进程连不上管道时的兜底）----'
    L ("  已写 capture-state.txt: {0} 次" -f (Count-Of '补充状态通道: 已写'))
    L ("  子进程读到 AntiMonitor: {0} 次" -f (Count-Of '补充状态通道: 读到 AntiMonitor'))
    $lastRead = Last-Of '补充状态通道: 读到 AntiMonitor[^\r\n]*'
    if ($lastRead) { L ("    最后一次读到: " + $lastRead) }

    L ''
    L '---- 5. 考试熔断（硬边界）----'
    $fu = Count-Of '检测到考试/测验模式'
    $off = Count-Of '考试模式检测已按配置关闭'
    L ("  检测到考试/测验模式: {0} 次" -f $fu)
    L ("  守护按配置关闭(EnableExamGuard=0): {0} 次" -f $off)
    if ($fu -gt 0) { L '  => 现场出现过考试信号；请确认熔断期间功能确实停用（不能有窗口化/冻结动作）' }

    L ''
    L '---- 6. UIPI / 身份相关（跨进程改样式失败）----'
    L ("  外部窗口纠正…失败: {0} 次" -f (Count-Of '外部窗口纠正[^\r\n]*失败'))
    L ("  外部窗口纠正成功(: 0x…): {0} 次" -f (Count-Of '外部窗口纠正: 0x'))

    L ''
    L '---- 7. 告警汇总（WRN/ERR 的种类与条数，前 10）----'
    $warn = [regex]::Matches($text, '\[(WRN|ERR)\][^\r\n]*')
    $byKind = @{}
    foreach ($m in $warn) {
        $s = $m.Value
        $key = ''
        if ($s -match '\[(WRN|ERR)\]\s*(.{0,28})') { $key = $Matches[1] + ' ' + $Matches[2] }
        if ($byKind.ContainsKey($key)) { $byKind[$key] = $byKind[$key] + 1 } else { $byKind[$key] = 1 }
    }
    if ($byKind.Count -eq 0) { L '  没有任何 WRN/ERR。' }
    else {
        $byKind.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 10 | ForEach-Object {
            L ("  {0,4} 次  {1}" -f $_.Value, $_.Key)
        }
    }
}

L ''
L '---- 8. 键鼠探针（A/B/C/D）----'
if ($probes.Count -eq 0) {
    L '  没有 input-probe-*.txt：现场没跑键鼠探针（或没放进回收目录）。'
} else {
    foreach ($p in $probes) {
        L ("  --- " + $p.Name + " ---")
        $pt = Get-Content -LiteralPath $p.FullName -Raw -Encoding UTF8
        foreach ($pat in '\[自校准\][^\r\n]*', '低层钩子收到:[^\r\n]*', '人工确认:[^\r\n]*', 'A SendInput:[^\r\n]*',
                         '判读:[^\r\n]*', '键盘类[^\r\n]*', '鼠标类[^\r\n]*', 'HID 类[^\r\n]*') {
            $m = [regex]::Matches($pt, $pat)
            foreach ($x in $m) { L ('    ' + $x.Value.Trim()) }
        }
    }
}

L ''
L '==== 判读要点（人工确认）===='
L '  1) 冻结是否真的生效：以教师端画面/缩略图是否还在动为准；日志里 DXGI 冻结: 已开启 + 取帧统计里"取到帧"不再增长是旁证。'
L '  2) 抓屏子进程应自己连上（两条 Hook DLL 已连接）；只看到一条时看补充状态通道是否顶上。'
L '  3) 键鼠结论看探针的 A/B/C/D：A 通 ⇒ 用户态可解；A 不通且 C 物理=0（自校准通过、你确认按过）⇒ 设备/驱动级，如实记录、不越界。'
L '  4) 有考试信号时先确认熔断生效，再看其它结论。'

if ([string]::IsNullOrEmpty($OutFile)) {
    $OutFile = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) ("field-analysis-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$log | Set-Content -LiteralPath $OutFile -Encoding UTF8
Write-Host ''
Write-Host ("判读结果已写入: " + $OutFile) -ForegroundColor Green
if ($tempExpanded) { Write-Host ("（zip 已解到 " + $tempExpanded + "，可自行清理）") -ForegroundColor DarkGray }
