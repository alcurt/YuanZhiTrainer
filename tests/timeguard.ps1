# 运行时间红线守门（用户 2026-10-12 设定）
#   中午场：13:55 必须停工，之后等用户说"复工"再继续
#   晚间场：22:45 必须停工，没跑完的留到第二天
# 用法：
#   pwsh -File tests\timeguard.ps1                 # 检查一次，打印剩余时间与档位
#   pwsh -File tests\timeguard.ps1 -Watch          # 守护：睡到红线后杀掉所有 YZ* 进程并留标记
# 退出码：0=OK（可跑长任务） 1=SHORT-ONLY（只做短操作） 2=STOP（已过红线，立刻停工）
param(
    [switch]$Watch,
    [int]$MinMinutes = 12
)

function Get-Deadline([datetime]$now) {
    $noon  = Get-Date -Year $now.Year -Month $now.Month -Day $now.Day -Hour 13 -Minute 55 -Second 0
    $night = Get-Date -Year $now.Year -Month $now.Month -Day $now.Day -Hour 22 -Minute 45 -Second 0
    if ($now -lt $noon)  { return $noon }
    if ($now -lt $night) { return $night }
    return $noon.AddDays(1)      # 过了 22:45 → 下一条红线是明天中午
}

$now = Get-Date
$dl  = Get-Deadline $now
$left = [int][Math]::Floor(($dl - $now).TotalMinutes)

if ($Watch) {
    "timeguard: 守护已启动，睡到 $($dl.ToString('MM-dd HH:mm'))"
    while ((Get-Date) -lt $dl) { Start-Sleep -Seconds 20 }
    $killed = @()
    foreach ($n in @('YZTrainer','YZSimTarget','YZSysRun','YZProbe')) {
        $ps = @(Get-Process -Name $n -ErrorAction SilentlyContinue)
        if ($ps.Count -gt 0) { $ps | Stop-Process -Force -ErrorAction SilentlyContinue; $killed += $n }
    }
    $mark = Join-Path $PSScriptRoot '..\build\logs\_STOP.txt'
    Set-Content -LiteralPath $mark -Encoding utf8 -Value ("红线到达 $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')；已清理进程: " + $(if($killed.Count){$killed -join ','}else{'无'}))
    "timeguard: 已到红线，清理进程: " + $(if($killed.Count){$killed -join ','}else{'无'})
    exit 2
}

"现在 $($now.ToString('HH:mm:ss'))｜下一条红线 $($dl.ToString('MM-dd HH:mm'))｜剩余 $left 分钟"
if ($left -le 0) {
    'STOP：已过红线 —— 立刻停工，把进度写进开发记录，等用户复工/次日再继续'
    exit 2
} elseif ($left -lt $MinMinutes) {
    "SHORT-ONLY：只剩 $left 分钟（< $MinMinutes）——只做短操作（提交/文档/清理），不要启动构建或整轮回归"
    exit 1
} else {
    "OK：剩 $left 分钟，可以启动长任务（构建约 1 分钟、整轮回归约 6 分钟）"
    exit 0
}