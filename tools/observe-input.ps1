# observe-input.ps1 —— 打开/关闭"只观测模式"（YZHook 的 InputObserveAll）
#
# 用途：现场查"谁在拽光标 / 谁在吞键盘"。默认我们的钩子只允许远志学生端与模拟目标启用，
# 别的进程（例如第二套远控的 ycdinfo.exe / Remote.exe）会被宿主闸门拒绝，于是一行日志都没有。
# 打开这个模式后，被点名的进程也会启用输入钩子，但**只记录、不拦截**。
#
# 用法（普通 PowerShell 即可）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\observe-input.ps1 -Names ycdinfo.exe,Remote.exe
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\observe-input.ps1 -Off
#
# 打开后跑一遍现场，然后在各进程的日志（或主程序日志里 [Hook pid=..] 转发段）搜：
#   [输入观测] 只观测模式启用（pid=.. exe=..）      ← 谁被点亮了
#   [输入取证] 本进程 1 秒内: SetCursorPos N 次（最后一次 x,y） ...  ← 谁在拽、拽到哪
param(
    [string]$Names = '',
    [switch]$Off
)

$path = 'C:\ProgramData\YZTrainer\observe-state.txt'
if ($Off) {
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force -ErrorAction SilentlyContinue
        Write-Host ('已关闭：删除 ' + $path)
    } else {
        Write-Host '本来就关着（文件不存在）'
    }
    return
}

if ([string]::IsNullOrWhiteSpace($Names)) {
    Write-Host '需要 -Names（逗号分隔的进程名），或 -Off 关闭。例如：' -ForegroundColor Yellow
    Write-Host '  powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\observe-input.ps1 -Names ycdinfo.exe,Remote.exe'
    return
}

$dir = Split-Path -Parent $path
if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
$content = "ObserveAll=1`r`nNames=" + ($Names -replace '\s', '') + "`r`n"
Set-Content -LiteralPath $path -Value $content -Encoding ASCII
Write-Host ('已打开只观测模式：' + $path) -ForegroundColor Green
Get-Content -LiteralPath $path | ForEach-Object { Write-Host ('  ' + $_) }
Write-Host '重启/重开主程序后生效；被点名的进程会在各自日志里打 [输入观测] 与 [输入取证]。'