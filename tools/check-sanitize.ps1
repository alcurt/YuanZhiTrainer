# 推送前脱敏扫描（2026-10-17 加；同日九轮修正 —— 第三至第九轮由推送会话复核驱动）
# 背景：v0.6.45–v0.6.48 推送时，发布 session 靠临时扫才发现 3 处机房机位名并额外做了脱敏提交；
#       更早提交里的机位名已进公开历史、不可撤销（用户知情接受）。只靠"推送前临时扫"不可靠 ⇒ 变成可重跑的一步。
#
# ==== 判定：拦截 vs 已登记 ====
#   拦截级（-Fail 时退出码 1）：
#     ① 当前树里的命中（**未登记**的白名单外）—— 改完文档先跑闸门就能拦住，避免"先跑闸门、再提交"把泄露带出去；
#     ② 未推送补丁 / 未推送提交信息 / 未公开 annotated tag 正文里的任何命中（= 新增泄露）。
#   只提示（不计入）：
#     · 已登记的历史遗留（tools\sanitize-allowlist.txt：文件 + 规则名 + 理由，**不写内容**）；
#     · 像版本号的四段串；· 已公开 tag 正文里的命中；· 已跟踪但读不到的文件（单独告警）。
#
# ==== 扫哪些输入 ====
#   A. 当前树（git -c core.quotepath=false ls-files）      B. 未推送提交信息
#   C. 未推送补丁（逐文件、跳过扫描器自身）                 D. 未公开 annotated tag 正文
#
# ==== 七类规则 ====
#   机位名       F + 三位数字 + - + 一或两位数字
#   IPv4(私网)   只判私网/回环/链路本地：10.x、172.16–31.x、192.168.x、127.x、169.254.x（四段、每段 0-255）
#   用户目录     X:\Users\<账户>，账户名只认字母数字._$-      MAC 地址 / 电子邮件 / DESKTOP- 主机名
#   疑似凭据     PRIVATE KEY，或 password/token/secret 等**赋值给带引号的字面量**
#
# ==== 计数恒等式（自证，别忽略）====
#   已跟踪 = 跳过自身 1 + 读不到 + 类型跳过 + 超大跳过 + 实扫
#   跑完会打印这五个数并判定是否恒等；不恒等会明说"自证不成立"。
#
# ==== 已知取舍（别当 bug 报）====
# 1. 整份跳过扫描器自身（规则与示例字样就在里面）；代价是该文件内的真实内容抓不到。
# 2. 四段串只对私网/回环/链路本地判命中，其它记为提示 —— 宁可漏一个公网 IP，也不误伤版本号。
# 3. 不检查"具体日期/时刻"（AGENTS §4 有约定，但已入库旧记录约 173 处，加规则必然全红，只靠人守）。
# 4. 机位名只认 F + 三位数字 的形态；别的编号体系不在覆盖内。
# 5. 已公开的 tag 正文不拦、只提示（其内容已在公开历史；照样拦会让闸门永久刷红）。
# 6. 提交信息与代码注释里都不要放具体实例（用文字描述），否则闸门会正确地抓自己。
# 7. 补丁里报的行号是**补丁行号**，不是文件行号（标为"补丁行 N"）。
# 8. `-NoMessages` 会连 B/C/D 一起关掉（只看当前树），排查时注意别误判。
# 9. 非 ASCII 路径必须配 -c core.quotepath=false：否则中文名文件对扫描**静默不可见**（5.1 下还会抛
#    Illegal characters in path）。本文件所有 git 调用都已带上，并用上面的计数恒等式自证。
#
# 用法：
#   pwsh -File tools\check-sanitize.ps1             # 报告，退出码 0
#   pwsh -File tools\check-sanitize.ps1 -Fail       # 有**拦截级**命中就退出码 1
#   pwsh -File tools\check-sanitize.ps1 -NoMessages # 只看当前树
param([switch]$Fail, [switch]$NoMessages)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$hits        = New-Object System.Collections.Generic.List[object]
$allowed     = New-Object System.Collections.Generic.List[object]
$hints       = New-Object System.Collections.Generic.List[string]
$unreadable  = New-Object System.Collections.Generic.List[string]
$skippedType = 0
$skippedSize = 0
$publicTagHits = 0

$rules = @(
    @{ Name = '机位名';     Re = 'F\d{3}-\d{1,2}';                     Mask = 'F***-**' },
    @{ Name = 'IPv4(私网)'; Re = '\b(?:\d{1,3}\.){3}\d{1,3}\b';        Mask = 'x.x.x.*' },
    @{ Name = '用户目录';   Re = '[A-Za-z]:\\Users\\[A-Za-z0-9._$-]{2,}'; Mask = 'X:\Users\<账户>' },
    @{ Name = 'MAC 地址';   Re = '\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b'; Mask = 'XX-XX-XX-XX-XX-XX' },
    @{ Name = '电子邮件';   Re = '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}'; Mask = 'name@host' },
    @{ Name = '主机名';     Re = '\bDESKTOP-[A-Za-z0-9]{5,}\b';        Mask = 'DESKTOP-*******' },
    @{ Name = '疑似凭据';   Re = '(PRIVATE KEY|(?i)\b(password|passwd|token|secret|api[_-]?key)\s*[=:]\s*["''][^"'']{3,}["''])'; Mask = '<凭据字样已遮罩>' }
)

# 返回对象数组：Where / Line / Rule / Mask / LineLabel
function Test-Text([string]$text, [string]$where, [string]$lineLabel = '行') {
    $found = New-Object System.Collections.Generic.List[object]
    $lineNo = 0
    foreach ($line in ($text -split "`r?`n")) {
        $lineNo++
        foreach ($r in $rules) {
            foreach ($mm in [regex]::Matches($line, $r.Re)) {
                if ($r.Name -eq 'IPv4(私网)') {
                    $ok = $true
                    foreach ($q in $mm.Value.Split('.')) { if ([int]$q -gt 255) { $ok = $false } }
                    if (-not $ok) { continue }
                    $parts = $mm.Value.Split('.')
                    $first = [int]$parts[0]; $second = [int]$parts[1]
                    $isPrivate = ($first -eq 10) -or ($first -eq 127) -or
                                 ($first -eq 192 -and $second -eq 168) -or
                                 ($first -eq 172 -and $second -ge 16 -and $second -le 31) -or
                                 ($first -eq 169 -and $second -eq 254)
                    if ($isPrivate) {
                        $found.Add([pscustomobject]@{ Where=$where; Line=$lineNo; LineLabel=$lineLabel; Rule=$r.Name; Mask=$r.Mask })
                    } else {
                        $script:hints.Add(('{0}:{1}  四段串（可能是版本号，不拦）' -f $where, $lineNo))
                    }
                } else {
                    $found.Add([pscustomobject]@{ Where=$where; Line=$lineNo; LineLabel=$lineLabel; Rule=$r.Name; Mask=$r.Mask })
                }
            }
        }
    }
    return $found
}

function Format-Hit($h) { $lab = $(if ($h.LineLabel -match '补丁') { '（补丁行）' } else { '' }); return ('{0}:{1}{2}  {3}（{4}）' -f $h.Where, $h.Line, $lab, $h.Rule, $h.Mask) }

# 读白名单（相对路径<TAB>规则名<TAB>理由）
$allowSet = @{}
$wlPath = Join-Path $PSScriptRoot 'sanitize-allowlist.txt'
if (Test-Path -LiteralPath $wlPath) {
    foreach ($ln in [System.IO.File]::ReadLines($wlPath)) {
        if ([string]::IsNullOrWhiteSpace($ln) -or $ln.TrimStart().StartsWith('#')) { continue }
        $f = $ln -split "`t"
        if ($f.Count -ge 2) { $allowSet[($f[0].Trim() + '|' + $f[1].Trim())] = $(if ($f.Count -ge 3) { $f[2].Trim() } else { '' }) }
    }
}

Push-Location -LiteralPath $root
try { $files = @(git -c core.quotepath=false ls-files) } finally { Pop-Location }
if ($files.Count -eq 0) { Write-Host '没有 git 跟踪的文件（不在仓库里？）'; exit 0 }

# ---------- A. 当前树 ----------
$scanned = 0
foreach ($rel in $files) {
    if ($rel -match '\.(exe|dll|png|jpg|jpeg|gif|ico|zip|7z|gz|pdf|obj|lib|pdb|ilk|exp|bin)$') { $skippedType++; continue }
    if ($rel -eq 'tools/check-sanitize.ps1') { continue }   # 取舍 1
    $full = Join-Path $root $rel
    try {
        if (-not (Test-Path -LiteralPath $full)) { $unreadable.Add($rel); continue }
        $info = Get-Item -LiteralPath $full -ErrorAction Stop
        if ($info.Length -gt 2MB) { $skippedSize++; continue }
        $scanned++
        foreach ($h in (Test-Text ([System.IO.File]::ReadAllText($full)) $rel '行')) {
            $key = $h.Where + '|' + $h.Rule
            if ($allowSet.ContainsKey($key)) { $allowed.Add($h) } else { $hits.Add($h) }
        }
    } catch { $unreadable.Add($rel) }
}

# ---------- B/C/D ----------
$msgScanned = 0
if (-not $NoMessages) {
    Push-Location -LiteralPath $root
    try {
        $hasOrigin = (git rev-parse --verify --quiet origin/master) 2>$null
        if (-not $hasOrigin) { $range = '-50' } else { $range = 'origin/master..HEAD' }
        foreach ($sha in @(git log $range --format='%H' 2>$null)) {
            if ([string]::IsNullOrWhiteSpace($sha)) { continue }
            $short = $sha.Substring(0,7)
            $msg = (git log -1 --format='%B' $sha 2>$null) -join [Environment]::NewLine
            $msgScanned++
            foreach ($h in (Test-Text $msg ('提交信息 ' + $short) '行')) { $hits.Add($h) }
            $changed = @(git -c core.quotepath=false show --name-only --format= $sha 2>$null) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
            foreach ($cf in $changed) {
                if ($cf -eq 'tools/check-sanitize.ps1') { continue }
                $patch = (git -c core.quotepath=false show --format= --patch $sha -- $cf 2>$null) -join [Environment]::NewLine
                if ([string]::IsNullOrWhiteSpace($patch)) { continue }
                $msgScanned++
                foreach ($h in (Test-Text $patch ('补丁 ' + $short + ' ' + $cf) '补丁行 ')) { $hits.Add($h) }
            }
        }
        foreach ($tg in @(git tag --list)) {
            $objType = (git for-each-ref --format='%(objecttype)' "refs/tags/$tg" 2>$null)
            if ($objType -ne 'tag') { continue }
            $body = git for-each-ref --format='%(contents)' "refs/tags/$tg" 2>$null
            if (-not $body) { continue }
            $sha = git rev-list -n 1 $tg 2>$null
            git merge-base --is-ancestor $sha origin/master 2>$null
            $isPublic = ($LASTEXITCODE -eq 0)
            $msgScanned++
            $tagHits = @(Test-Text (($body -join [Environment]::NewLine)) ('tag ' + $tg) '行')
            if ($tagHits.Count -eq 0) { continue }
            if ($isPublic) { $publicTagHits += $tagHits.Count } else { foreach ($h in $tagHits) { $hits.Add($h) } }
        }
    } finally { Pop-Location }
}

# ---------- 计数恒等式自证 ----------
$expr = $files.Count - ($scanned + $unreadable.Count + $skippedType + $skippedSize + 1)
Write-Host ('==== 脱敏扫描：已跟踪 {0} = 自身 1 + 读不到 {1} + 类型跳过 {2} + 超大跳过 {3} + 实扫 {4}（差 {5}）====' -f $files.Count, $unreadable.Count, $skippedType, $skippedSize, $scanned, $expr)
if ($expr -ne 0) { Write-Host '警告: 计数恒等式不成立 ⇒ 页面自证失效，请查扫描逻辑' }
Write-Host ('文本块 {0} 个（提交信息/补丁/tag）；规则 {1} 类' -f $msgScanned, $rules.Count)
if ($unreadable.Count -gt 0) {
    Write-Host '警告: 有已跟踪文件读不到，闸门对它们不可见：'
    $unreadable | ForEach-Object { Write-Host ('  ' + $_) }
}
if ($hints.Count -gt 0)       { Write-Host ('提示: {0} 处四段串看着像版本号（不拦）' -f $hints.Count) }
if ($publicTagHits -gt 0)     { Write-Host ('提示: 已公开的 tag 正文里有 {0} 处命中（不可补救，不计入闸门）' -f $publicTagHits) }
if ($allowed.Count -gt 0) {
    Write-Host ('已登记的历史遗留 {0} 处（白名单 tools\sanitize-allowlist.txt，不拦）：' -f $allowed.Count)
    $allowed | ForEach-Object { Write-Host ('  ' + (Format-Hit $_)) }
}
if ($hits.Count -eq 0) {
    Write-Host '拦截级结果: 无命中（当前树 + 未推送补丁/提交信息/tag 正文）'
    if ($Fail -and ($unreadable.Count -gt 0 -or $expr -ne 0)) { exit 1 }
    exit 0
}
Write-Host ('拦截级命中: {0} 处 —— 推送前必须脱敏（当前树未登记 或 新增泄露）：' -f $hits.Count)
$hits | ForEach-Object { Write-Host ('  ' + (Format-Hit $_)) }
if ($Fail) { exit 1 }
exit 0