# YZTrainer 现场数据回收（v0.6.40 起随现场清单使用）
#
# 目的：从机房回来时**别漏东西**。踩过的坑：
#   1) 只看 yzt.log 会漏掉真正活跃的那份——产品按 2MB 顺延编号（yzt.log / yzt-1.log / yzt-2.log …），
#      机房跑一节课下来当前活跃的很可能是 -1 或 -2；
#   2) 主程序以 SYSTEM 身份跑过时，日志落在 C:\Windows\Temp\YZTrainer 而不是用户 %TEMP%\YZTrainer；
#   3) 诊断包（diag-*）本身就自带各来源日志，但经常只拷了其中一份。
#
# 用法（普通 PowerShell 即可，5.1 与 pwsh 7 都行）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\collect-field-data.ps1
#   可选：-OutDir D:\带回 -Zip:$false
#
# 产出：<OutDir>\field-<时间戳>\ 目录（含 manifest.txt）与同名 .zip

param(
    [string]$OutDir = '',
    [switch]$NoZip
)

$ErrorActionPreference = 'Continue'
if ([string]::IsNullOrEmpty($OutDir)) { $OutDir = (Get-Location).Path }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$dest = Join-Path $OutDir ("field-{0}" -f $stamp)
New-Item -ItemType Directory -Path $dest -Force | Out-Null

# 有些目录（例如 SYSTEM 身份跑过时留下的 C:\Windows\Temp\YZTrainer）在非管理员下会直接抛
# 权限异常；这里统一兜住，只记一条提示，不让错误刷屏。
function Test-DirSafe([string]$path) {
    # 注意：Test-Path 在无权限目录上抛的是**非终止错误**，try/catch 挡不住，必须给 -ErrorAction
    try { return (Test-Path -LiteralPath $path -ErrorAction SilentlyContinue) } catch { return $false }
}
$manifest = New-Object System.Collections.Generic.List[string]
function M([string]$s) { $manifest.Add($s); Write-Host $s }

M ("==== YZTrainer 现场数据回收 {0} ====" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
M ("主机: {0}  用户: {1}  管理员: {2}" -f $env:COMPUTERNAME, $env:USERNAME,
   ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
M ''

# 可能放日志/诊断包的目录（存在就扫）
$roots = @(
    (Join-Path $env:TEMP 'YZTrainer'),
    (Join-Path $env:SystemRoot 'Temp\YZTrainer'),
    'C:\ProgramData\YZTrainer',
    (Get-Location).Path,
    (Split-Path -Parent $MyInvocation.MyCommand.Path)
) | Sort-Object -Unique

$copied = 0
function Copy-Into([string]$src, [string]$sub) {
    if (-not (Test-Path -LiteralPath $src)) { return }
    $target = Join-Path $dest $sub
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    try {
        Copy-Item -LiteralPath $src -Destination $target -Recurse -Force -ErrorAction Stop
        $script:copied++
        $item = Get-Item -LiteralPath $src
        M ("  + {0}  ({1})" -f $src, $(if ($item.PSIsContainer) { '目录' } else { "$($item.Length) 字节" }))
    } catch {
        M ("  ! 复制失败: {0}  ({1})" -f $src, $_.Exception.Message)
    }
}

M '---- 日志（含轮转档）----'
foreach ($r in $roots) {
    if (-not (Test-DirSafe $r)) { M ("  （跳过，无权限或无此目录: {0}）" -f $r); continue }
    $tag = ($r -replace '[:\\]', '_')
    foreach ($pat in 'yzt*.log', 'remote-exec*.log', 'exam-debug.txt') {
        Get-ChildItem -LiteralPath $r -Filter $pat -File -ErrorAction SilentlyContinue |
            ForEach-Object { Copy-Into $_.FullName (Join-Path 'logs' $tag) }
    }
}

M ''
M '---- 诊断包（diag-*）----'
foreach ($r in $roots) {
    if (-not (Test-DirSafe $r)) { M ("  （跳过，无权限或无此目录: {0}）" -f $r); continue }
    Get-ChildItem -LiteralPath $r -Directory -Filter 'diag-*' -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Into $_.FullName 'diag' }
}

M ''
M '---- 探针产物（watch / report / 输入探针）----'
foreach ($r in $roots) {
    if (-not (Test-DirSafe $r)) { M ("  （跳过，无权限或无此目录: {0}）" -f $r); continue }
    foreach ($pat in 'YZProbe-report-*.*', 'YZProbe-watch-*.log', 'input-probe-*.txt', 'probe-*.log') {
        Get-ChildItem -LiteralPath $r -Filter $pat -File -ErrorAction SilentlyContinue |
            ForEach-Object { Copy-Into $_.FullName 'probe' }
    }
}

M ''
M '---- 配置文件（判读上下文，必须有）----'
foreach ($r in $roots) {
    if (-not (Test-DirSafe $r)) { M ("  （跳过，无权限或无此目录: {0}）" -f $r); continue }
    Get-ChildItem -LiteralPath $r -Filter 'YZTrainer.ini' -File -ErrorAction SilentlyContinue |
        ForEach-Object { Copy-Into $_.FullName 'ini' }
}

M ''
M '---- 主日志指纹（带回后核对用）----'
foreach ($f in (Get-ChildItem -LiteralPath $dest -Recurse -Filter 'yzt*.log' -File -ErrorAction SilentlyContinue)) {
    $h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
    M ("  {0}  {1} 字节  SHA256={2}" -f $f.FullName.Substring($dest.Length + 1), $f.Length, $h.Substring(0, 16))
}

M ''
M ("共复制 {0} 项 ⇒ {1}" -f $copied, $dest)
$manifest | Set-Content -LiteralPath (Join-Path $dest 'manifest.txt') -Encoding UTF8

if (-not $NoZip) {
    $zip = "$dest.zip"
    try {
        if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
        Compress-Archive -Path (Join-Path $dest '*') -DestinationPath $zip -Force
        Write-Host ("打包: " + $zip) -ForegroundColor Green
    } catch {
        Write-Host ("打包失败（数据仍在目录里）: " + $_.Exception.Message) -ForegroundColor Yellow
    }
}
Write-Host ''
Write-Host '带回前请确认：manifest.txt 里"日志（含轮转档）"至少 1 份、"诊断包"含广播/采集/退出三份。' -ForegroundColor Cyan
