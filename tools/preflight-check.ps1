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
    [string]$ExpectVersion = '',      # 留空＝自动从 src\common\yz_version.h 读（免得每次升版本都要改这个脚本）
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

# 版本期望值：优先用参数，其次从源码读，读不到就跳过版本比较（以 dist 自身为准）。
if ([string]::IsNullOrEmpty($ExpectVersion)) {
    $vh = Join-Path $repo 'src\common\yz_version.h'
    if (Test-Path -LiteralPath $vh -ErrorAction SilentlyContinue) {
        $m = Select-String -LiteralPath $vh -Pattern 'YZ_VERSION_STR_NARROW\s+"([0-9.]+)"' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($m) { $ExpectVersion = $m.Matches[0].Groups[1].Value }
    }
}
L ("==== YZTrainer 出发前检查 {0}（期望版本 {1}）====" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $(if ($ExpectVersion) { $ExpectVersion } else { '（未能确定，按 dist 自身为准）' }))

L ''
L '---- 1. 交付产物 ----'
# v0.6.40 修订（2026-10-15 现场教训）：用户是把 dist 里的两个平台目录直接放到 YZ\ 下的
# （YZ\Win32、YZ\x64），不是 YZ\dist\Win32 ⇒ 两种布局都要认，否则"文件齐全却报缺失"。
function Get-PlatformDir([string]$repo, [string]$pl) {
    $a = Join-Path $repo ("dist\{0}" -f $pl)
    if (Test-Path -LiteralPath (Join-Path $a 'YZTrainer.exe') -ErrorAction SilentlyContinue) { return $a }
    $b = Join-Path $repo $pl
    if (Test-Path -LiteralPath (Join-Path $b 'YZTrainer.exe') -ErrorAction SilentlyContinue) { return $b }
    return $a   # 两个都没有时仍返回 dist 路径，让后面的检查报缺失
}
foreach ($pl in 'Win32', 'x64') {
    $platDir = Get-PlatformDir $repo $pl
    $ex = Join-Path $platDir 'YZTrainer.exe'
    $it = Get-Item -LiteralPath $ex -ErrorAction SilentlyContinue
    $verOk = if ($it -and $ExpectVersion) { $it.VersionInfo.FileVersionRaw -eq ("$ExpectVersion.0") } elseif ($it) { $true } else { $false }
    Check ("$pl YZTrainer.exe") $verOk `
          ("版本=" + $(if ($it) { $it.VersionInfo.FileVersionRaw } else { '缺失' }) + " " + $(if ($it) { "$($it.Length) 字节" }))
    Check ("$pl YZProbe.exe") (Test-Path -LiteralPath (Join-Path $platDir 'YZProbe.exe') -ErrorAction SilentlyContinue) ''
    $ini = Join-Path $platDir 'YZTrainer.ini'
    # 开发机上是出厂值 Flags=5；U 盘/现场会把 tools\field-ini 的模板覆盖进来（Flags=45），两者都合格。
    $flags = (Select-String -LiteralPath $ini -Pattern '^Flags=(\d+)' -ErrorAction SilentlyContinue | Select-Object -First 1)
    # 现场那份 ini 可能被产品按用户操作重写过（例如只开键鼠解锁+防监视 ⇒ Flags=12），
    # 所以这里只确认"存在且 Flags 可读"，并把含义写出来供人核对，不把非 5/45 当失败。
    $fv = if ($flags) { $flags.Matches[0].Groups[1].Value } else { '' }
    $meaning = ''
    if ($fv -ne '') {
        $v = [int]$fv
        $parts = @()
        if ($v -band 1)  { $parts += '窗口化' }
        if ($v -band 4)  { $parts += '键鼠解锁' }
        if ($v -band 8)  { $parts += '防监视' }
        if ($v -band 16) { $parts += '拦遥控' }
        if ($v -band 32) { $parts += '假全屏' }
        $meaning = if ($parts.Count -gt 0) { '（' + ($parts -join '+') + '）' } else { '（无功能位）' }
    }
    Check ("$pl ini") ($fv -ne '') ("Flags=$fv $meaning" + $(if ($fv -eq '5') { ' 出厂值' } elseif ($fv -eq '45') { ' 现场模板' } else { '' }))
}

L ''
L '---- 1b. dist 里不应有回归残留（否则会被一起拷进 U 盘，判读时干扰计数）----'
foreach ($pl in 'Win32', 'x64') {
    $dirs = @(Get-ChildItem -LiteralPath $platDir -Directory -Filter 'diag-*' -ErrorAction SilentlyContinue)
    $junk = @(Get-ChildItem -LiteralPath $platDir -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -like 'sim_*' -or $_.Name -like '*.bak*' })
    Check ("$pl 无 diag-* 残留") ($dirs.Count -eq 0) ("发现 {0} 个诊断包目录（跑过回归就会有；拷走前清一下）" -f $dirs.Count)
    Check ("$pl 无 sim_*/*.bak* 残留") ($junk.Count -eq 0) ("发现 {0} 个" -f $junk.Count)
}
L ''
L '---- 1c. 出厂 ini 良构性与两侧一致（v0.6.42 踩过：build.ps1 里字符串被插穿成多行，语法检查查不出来）----'
foreach ($pl in 'Win32', 'x64') {
    $ini2 = Join-Path (Get-PlatformDir $repo $pl) 'YZTrainer.ini'
    if (-not (Test-Path -LiteralPath $ini2 -ErrorAction SilentlyContinue)) { Check "$pl ini 存在" $false '文件缺失'; continue }
    $pfBadLines = @()
    foreach ($pfLn in (Get-Content -LiteralPath $ini2 -Encoding UTF8)) {
        $t2 = $pfLn.Trim()
        if ($t2 -eq '') { continue }
        if ($t2 -match '^[;#]') { continue }                 # 注释
        if ($t2 -match '^\[[^\]]+\]$') { continue }           # 段
        if ($t2 -match '^[A-Za-z][A-Za-z0-9_]*=.*$') { continue }   # 键值（键必须单行、无内嵌换行）
        $pfBadLines += $t2
    }
    Check ("$pl ini 良构") ($pfBadLines.Count -eq 0) $(if ($pfBadLines.Count) { '可疑行: ' + (($pfBadLines | Select-Object -First 2) -join ' | ') } else { '每行都是 段/注释/键值' })
    $need = @('Flags', 'TraceDesktopDuplication', 'InjectCaptureChild', 'CaptureScanIntervalMs')
    $miss = @($need | Where-Object { -not (Select-String -LiteralPath $ini2 -Pattern "^$_=" -ErrorAction SilentlyContinue) })
    Check ("$pl ini 含关键键") ($miss.Count -eq 0) $(if ($miss.Count) { '缺: ' + ($miss -join ', ') } else { '四个关键键都在' })
    # "字符串被插穿成多行"那类损坏，在文件层面的真实特征是**同一个键出现两次**
    # （坏元素里含嵌换行 ⇒ 展开成多行，同时原有的同名元素还在）。所以这里查重复键。
    $pfKeys = @()
    foreach ($pfLn in (Get-Content -LiteralPath $ini2 -Encoding UTF8)) {
        $pfT = $pfLn.Trim()
        if ($pfT -match '^([A-Za-z][A-Za-z0-9_]*)=(.*)$') { $pfKeys += $Matches[1] }
    }
    $pfDup = @($pfKeys | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { "$($_.Name)×$($_.Count)" })
    Check ("$pl ini 无重复键") ($pfDup.Count -eq 0) $(if ($pfDup.Count) { '重复: ' + ($pfDup -join ', ') } else { "$($pfKeys.Count) 个键各不相同" })
}
# exe 里必须真的内建了 v0.6.42 的出厂模板（宽字符串以 UTF-16LE 落在二进制里）
Check 'exe 内建出厂模板含 CaptureScanIntervalMs' (HasStr (Join-Path (Get-PlatformDir $repo 'Win32') 'YZTrainer.exe') 'CaptureScanIntervalMs=30000') ''
L ''
L '---- 2. 内嵌钩子载荷是否为当前源码（v0.6.35 踩过旧载荷）----'
$mainExe = Join-Path (Get-PlatformDir $repo 'Win32') 'YZTrainer.exe'
Check '内嵌载荷含"补充状态通道"' (HasStr $mainExe '补充状态通道') ''
Check '内嵌载荷含"[DXGI 判定]"'   (HasStr $mainExe '[DXGI 判定]') ''

L ''
L '---- 3. 现场工具（pwsh7 与 5.1 都要能解析）----'
foreach ($t in @('tools\input-probe.ps1', 'tools\collect-field-data.ps1', 'tools\analyze-field-data.ps1', 'tests\timeguard.ps1')) {
    $full = Join-Path $repo $t
    if (-not (Test-Path -LiteralPath $full -ErrorAction SilentlyContinue)) {
        # 只带 dist+tools 的 U 盘/现场布局没有 tests 目录，这属正常（回归只在开发机上跑），记跳过不算失败。
        if ($t -like 'tests\*') { $ok++; L ("  [OK]   {0}  （跳过：未带 tests 目录，现场不需要它）" -f $t) }
        else { Check $t $false '文件缺失' }
        continue
    }
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
# U 盘/拷贝出来的目录不是 git 仓库，这一项要跳过而不是报 BAD（否则现场会误以为检查没过）。
if (Test-Path -LiteralPath (Join-Path $repo '.git') -ErrorAction SilentlyContinue) {
    $st = @(git -C $repo status --porcelain 2>$null)
    Check 'git 工作区干净' ($st.Count -eq 0) ($st -join ' ')
} else {
    $ok++; L '  [OK]   git 工作区干净  （跳过：当前目录不是 git 仓库，从 U 盘/拷贝目录运行属正常）'
}
$procs = @(Get-Process -Name YZTrainer, YZSimTarget, YZProbe, YZSysRun -ErrorAction SilentlyContinue)
Check '没有残留进程' ($procs.Count -eq 0) ($procs | ForEach-Object { $_.ProcessName } | Select-Object -Unique)

L ''
L ("结论: OK={0}  BAD={1}" -f $ok, $bad)
if ($bad -gt 0) { L '有 BAD 项：先修掉再出发（别带着不确定的包上机）。' }
$R | Set-Content -LiteralPath $out -Encoding UTF8
Write-Host ("记录: " + $out)
if ($bad -gt 0) { exit 1 }
exit 0
