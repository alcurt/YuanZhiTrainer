# YZTrainer 出发前检查（v0.6.40 起随现场清单使用）
#
# 目的：每次上机前跑一遍，把"带错包/旧载荷/编码坑"这类会白跑一趟的问题挡在本地。
#   检查项：交付产物版本与出厂 ini、内嵌钩子载荷是否为当前源码（v0.6.35 踩过"上一版载荷"）、
#           现场工具在 pwsh7 与 5.1 双解析器下是否都能跑、现场 ini 是否**无 BOM**
#           （带 BOM 会让 GetPrivateProfileInt 读不到 [General]，整份配置静默失效）、
#           git 是否干净、有没有残留进程。
#
# 用法（5.1 或 pwsh 7 均可）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\preflight-check.ps1
# 可选：-ExpectVersion 0.6.40
#
# 退出码：0 = 全绿；1 = 有 BAD 项（报告会写明是哪一项）

param(
    [string]$ExpectVersion = '0.6.40',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if ([string]::IsNullOrEmpty($OutDir)) { $OutDir = Join-Path $repo 'build\logs' }
if (-not (Test-Path -LiteralPath $OutDir -ErrorAction SilentlyContinue)) {
    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
}
$out = Join-Path $OutDir ("preflight-{0}.log" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
$R = New-Object System.Collections.Generic.List[string]
$ok = 0; $bad = 0
function L([string]$s) { $R.Add($s); Write-Host $s }
function Check([string]$name, [bool]$cond, [string]$detail) {
    if ($cond) { $script:ok++; L ("  [OK]   {0}  {1}" -f $name, $detail) }
    else       { $script:bad++; L ("  [BAD]  {0}  {1}" -f $name, $detail) }
}
# 在文件内容里找 UTF-16LE 字符串（判断内嵌载荷是否含某段文案）
function HasStr([string]$path, [string]$needle) {
    if (-not (Test-Path -LiteralPath $path -ErrorAction SilentlyContinue)) { return $false }
    $b = [System.IO.File]::ReadAllBytes($path)
    $p = [System.Text.Encoding]::Unicode.GetBytes($needle)
    for ($i = 0; $i -le $b.Length - $p.Length; $i++) {
        $eq = $true
        for ($j = 0; $j -lt $p.Length; $j++) { if ($b[$i + $j] -ne $p[$j]) { $eq = $false; break } }
        if ($eq) { return $true }
    }
    return $false
}
function ParseErrors([string]$path) {
    $e = $null
    [void][System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$null, [ref]$e)
    return @($e).Count
}

L ("==== YZTrainer 出发前检查 {0}（期望版本 {1}）====" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $ExpectVersion)

L ''
L '---- 1. 交付产物 ----'
foreach ($pl in 'Win32', 'x64') {
    $ex = Join-Path $repo ("dist\{0}\YZTrainer.exe" -f $pl)
    $it = Get-Item -LiteralPath $ex -ErrorAction SilentlyContinue
    Check ("$pl YZTrainer.exe") ($it -and $it.VersionInfo.FileVersionRaw -eq ("$ExpectVersion.0")) `
          ("版本=" + $(if ($it) { $it.VersionInfo.FileVersionRaw } else { '缺失' }) + " " + $(if ($it) { "$($it.Length) 字节" }))
    Check ("$pl YZProbe.exe") (Test-Path -LiteralPath (Join-Path $repo "dist\$pl\YZProbe.exe") -ErrorAction SilentlyContinue) ''
    $ini = Join-Path $repo "dist\$pl\YZTrainer.ini"
    $flags = (Select-String -LiteralPath $ini -Pattern '^Flags=(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1)
    Check ("$pl 出厂 ini") ($flags -and $flags.Matches[0].Groups[1].Value -eq '5') 'Flags=5（出厂值）'
}

L ''
L '---- 2. 内嵌钩子载荷是否为当前源码（v0.6.35 踩过旧载荷）----'
Check '内嵌载荷含"补充状态通道"' (HasStr (Join-Path $repo 'dist\Win32\YZTrainer.exe') '补充状态通道') ''
Check '内嵌载荷含"[DXGI 判定]"'   (HasStr (Join-Path $repo 'dist\Win32\YZTrainer.exe') '[DXGI 判定]') ''

L ''
L '---- 3. 现场工具（pwsh7 与 5.1 都要能解析）----'
foreach ($t in @('tools\input-probe.ps1', 'tools\collect-field-data.ps1', 'tools\analyze-field-data.ps1', 'tests\timeguard.ps1')) {
    $full = Join-Path $repo $t
    if (-not (Test-Path -LiteralPath $full -ErrorAction SilentlyContinue)) { Check $t $false '文件缺失'; continue }
    $e7 = ParseErrors $full
    $e51 = 0
    try {
        $tmp = [System.IO.Path]::GetTempFileName()
        $cmd = "`$e=`$null; [void][System.Management.Automation.Language.Parser]::ParseFile('$full',[ref]`$null,[ref]`$e); @(`$e).Count | Set-Content -LiteralPath '$tmp' -Encoding ASCII"
        powershell.exe -NoProfile -Command $cmd | Out-Null
        $e51 = [int](Get-Content -LiteralPath $tmp -ErrorAction SilentlyContinue | Select-Object -First 1)
        Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    } catch { $e51 = -1 }
    Check $t (($e7 -eq 0) -and ($e51 -eq 0)) ("pwsh7=$e7  5.1=$e51 解析错")
}

L ''
L '---- 4. 现场 ini 必须无 BOM（否则整份配置静默失效）----'
$fin = Join-Path $repo 'tools\field-ini\YZTrainer.ini'
if (Test-Path -LiteralPath $fin -ErrorAction SilentlyContinue) {
    $b = [System.IO.File]::ReadAllBytes($fin)
    $bom = ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
    Check 'field-ini 无 BOM' (-not $bom) ('前 3 字节=' + ('{0:X2}{1:X2}{2:X2}' -f $b[0], $b[1], $b[2]))
    foreach ($k in 'Flags', 'TraceDesktopDuplication', 'InjectCaptureChild', 'CaptureScanIntervalMs') {
        Check ("field-ini 含 $k") ((Select-String -LiteralPath $fin -Pattern "^$k=" -ErrorAction SilentlyContinue) -ne $null) ''
    }
} else {
    Check 'tools\field-ini\YZTrainer.ini' $false '文件缺失'
}

L ''
L '---- 5. 工作区状态 ----'
$st = @(git -C $repo status --porcelain 2>$null)
Check 'git 工作区干净' ($st.Count -eq 0) ($st -join ' ')
$procs = @(Get-Process -Name YZTrainer, YZSimTarget, YZProbe, YZSysRun -ErrorAction SilentlyContinue)
Check '没有残留进程' ($procs.Count -eq 0) ($procs | ForEach-Object { $_.ProcessName } | Select-Object -Unique)

L ''
L ("结论: OK={0}  BAD={1}" -f $ok, $bad)
if ($bad -gt 0) { L '有 BAD 项：先修掉再出发（别带着不确定的包上机）。' }
$R | Set-Content -LiteralPath $out -Encoding UTF8
Write-Host ("记录: " + $out)
if ($bad -gt 0) { exit 1 }
exit 0
