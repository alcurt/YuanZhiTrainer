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

# v0.6.40 修订（2026-10-15 现场教训）：只在"当前目录"扫是不够的——把工具放在 YZ\tools、
# 从 YZ\ 里运行脚本时，诊断包与探针报告其实都在 YZ\Win32\ （exe 所在目录）⇒ 会把它们漏掉。
# 所以这里再把**直接子目录**里"看起来像 exe 目录"的（含 YZTrainer.exe 或任何 diag-*）也纳入扫描。
$subDirs = @()
foreach ($sub in (Get-ChildItem -Directory -ErrorAction SilentlyContinue)) {
    $looksLikeExeDir = (Test-Path -LiteralPath (Join-Path $sub.FullName 'YZTrainer.exe') -ErrorAction SilentlyContinue) -or
                       (Test-Path -LiteralPath (Join-Path $sub.FullName 'YZTrainer.ini') -ErrorAction SilentlyContinue) -or
                       (@(Get-ChildItem -LiteralPath $sub.FullName -Directory -Filter 'diag-*' -ErrorAction SilentlyContinue).Count -gt 0)
    if ($looksLikeExeDir) { $subDirs += $sub.FullName }
}
if ($subDirs.Count -gt 0) {
    M ("（自动补扫 {0} 个直接子目录：{1}）" -f $subDirs.Count, (($subDirs | ForEach-Object { Split-Path $_ -Leaf }) -join ', '))
    $roots = @($roots + $subDirs) | Sort-Object -Unique
}

$copied = 0
function Copy-Into([string]$src, [string]$sub, [string]$asName = '') {
    if (-not (Test-Path -LiteralPath $src)) { return }
    $target = Join-Path $dest $sub
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    # $asName 非空时改用它作为目标文件名（用于同名文件加来源前缀，避免互相覆盖）
    $dst = if ($asName) { Join-Path $target $asName } else { $target }
    # v0.6.47（工具）：按内容哈希去重。同一份产物（探针报告、快照等）常常同时躺在 <U 盘根>、
    #   <平台子目录> 与 diag-* 诊断包里，逐一收进来会让包膨胀、清单也显得“有很多份”。
    #   只跳过逐字节相同的文件；目录不参与。被跳过的条数在清单末尾汇总。
    if ($null -eq $script:seenHashes) { $script:seenHashes = @{}; $script:dupSkipped = 0 }
    if (-not (Get-Item -LiteralPath $src).PSIsContainer) {
        $h = (Get-FileHash -LiteralPath $src -Algorithm SHA256 -ErrorAction SilentlyContinue).Hash
        if ($h) {
            if ($script:seenHashes.ContainsKey($h)) {
                $script:dupSkipped++
                M ("  = 跳过重复内容: {0}（与已收的 {1} 逐字节相同）" -f $src, $script:seenHashes[$h])
                return
            }
            $script:seenHashes[$h] = $src
        }
    }
    try {
        Copy-Item -LiteralPath $src -Destination $dst -Recurse -Force -ErrorAction Stop
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
    Get-ChildItem -LiteralPath $r -Filter 'YZTrainer.ini' -File -ErrorAction SilentlyContinue | ForEach-Object {
        # v0.6.42 修订：两份同名 ini 扁平化会互相覆盖——2026-10-15 下午实测到 x64 的出厂 ini 把
        # Win32 的现场 ini 盖掉了，判读里于是显示 Flags=5 而不是现场实际用的 Flags=45。
        # 这里按来源目录加前缀，两份都留得住。
        $tag = (Split-Path $r -Leaf) -replace '[^\w\.\-]', '_'
        Copy-Into $_.FullName 'ini' ('{0}_YZTrainer.ini' -f $tag)
    }
}

M ''
M '---- 主日志指纹（带回后核对用）----'
foreach ($f in (Get-ChildItem -LiteralPath $dest -Recurse -Filter 'yzt*.log' -File -ErrorAction SilentlyContinue)) {
    $h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash
    M ("  {0}  {1} 字节  SHA256={2}" -f $f.FullName.Substring($dest.Length + 1), $f.Length, $h.Substring(0, 16))
}

M ''
M ("共复制 {0} 项 ⇒ {1}" -f $copied, $dest)
M ("（另有 {0} 项因内容逐字节相同被跳过——同一份产物在多个来源目录里各有一份，清单里不再重复列）" -f $script:dupSkipped)
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
# 防漏：一个 diag 都没收到，往往是因为**没在放 exe/diag 的目录里跑**（诊断包生成在 exe 所在目录）。
$diagCount = @(Get-ChildItem -LiteralPath (Join-Path $dest 'diag') -Directory -ErrorAction SilentlyContinue).Count
if ($diagCount -eq 0) {
    Write-Host '提示：没找到任何 diag-* 诊断包。诊断包生成在**主程序 exe 所在的目录**，' -ForegroundColor Yellow
    Write-Host '      请 cd 到那个目录再跑本脚本（或用 -OutDir 指定，但扫描目录仍是"当前目录"）。' -ForegroundColor Yellow
} else {
    Write-Host ("已收到 {0} 个诊断包（期望含 广播 / 采集 / 退出 三份，按现场实际发生的取）。" -f $diagCount) -ForegroundColor Cyan
}
Write-Host '带回前请确认：manifest.txt 里"日志（含轮转档）"至少 1 份、诊断包数与你现场触发的次数对得上。' -ForegroundColor Cyan
