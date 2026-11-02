# screen-baseline.ps1 —— 现场屏幕几何基线（一次性打印，供现场备注引用）
#
# 为什么需要：厂商抓屏与我们的替换都按"虚拟屏幕矩形"算（SM_XVIRTUALSCREEN 那一组）。
# 多显示器、扩展模式、125%/175% 缩放都会让"逻辑坐标"与"物理像素"不一致；
# 一旦对不上，"冻不上"到底是几何问题还是逻辑问题，事后从日志里分不出来。
#
# 用法（5.1 与 7.x 都行，不需要管理员）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\screen-baseline.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\screen-baseline.ps1 -Out D:\带回\屏幕基线.txt
#
# 两个坑（都实测踩过）：
#   1) 不要写成 `powershell -Command "… $_ …"` 的嵌套单行 —— 外层 PowerShell 先把 $_ 展开成空，
#      报"必须在 -f 运算符后面提供一个值表达式"；5.1 与 7.x 表现一致。
#   2) 本文件必须存成 UTF-8 **带 BOM** —— 5.1 对无 BOM 的 .ps1 按 ANSI 读，中文被拆开后
#      某字节等于 0x27(') 会提前终止字符串，报 "The string is missing the terminator"。

param([string]$Out = '')

$lines = New-Object System.Collections.Generic.List[string]
function Add-Line([string]$s) { [void]$script:lines.Add($s) }

Add-Line ('时间: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Add-Line ('主机: ' + $env:COMPUTERNAME + '   用户: ' + $env:USERNAME)
Add-Line ('PowerShell: ' + $PSVersionTable.PSVersion.ToString() + '   64位=' + [Environment]::Is64BitProcess)
Add-Line ''

$vw = 0
$vh = 0
$sigUser = '[DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);'
try {
    Add-Type -Namespace YzScr -Name Metrics -MemberDefinition $sigUser -ErrorAction Stop
    $vx = [YzScr.Metrics]::GetSystemMetrics(76)
    $vy = [YzScr.Metrics]::GetSystemMetrics(77)
    $vw = [YzScr.Metrics]::GetSystemMetrics(78)
    $vh = [YzScr.Metrics]::GetSystemMetrics(79)
    $mw = [YzScr.Metrics]::GetSystemMetrics(0)
    $mh = [YzScr.Metrics]::GetSystemMetrics(1)
    Add-Line ('虚拟屏幕: {0},{1} {2}x{3}' -f $vx, $vy, $vw, $vh)
    Add-Line ('主显示器: {0}x{1}' -f $mw, $mh)
} catch {
    Add-Line ('虚拟屏幕: 读取失败 ' + $_.Exception.Message)
}

try {
    Add-Type -AssemblyName System.Windows.Forms -ErrorAction Stop
    $i = 0
    foreach ($s in [System.Windows.Forms.Screen]::AllScreens) {
        $i++
        Add-Line ('显示器 {0}: {1} {2}x{3} @ {4},{5} 主={6}' -f `
            $i, $s.DeviceName, $s.Bounds.Width, $s.Bounds.Height, $s.Bounds.X, $s.Bounds.Y, $s.Primary)
    }
    Add-Line ('显示器数量: ' + $i)
} catch {
    Add-Line ('显示器枚举: 失败 ' + $_.Exception.Message)
}

try {
    $c = Get-CimInstance Win32_VideoController -ErrorAction Stop
    foreach ($g in $c) {
        Add-Line ('显卡: {0} {1}x{2}' -f $g.Name, $g.CurrentHorizontalResolution, $g.CurrentVerticalResolution)
    }
} catch {
    Add-Line '显卡: 读取失败'
}

Add-Line ''
# 缩放比：以用户设置为准。注意 GetDeviceCaps(LOGPIXELSX) 在本类"非 DPI 感知"进程里恒返回 96，不能用它判缩放。
$ratio = 1.0
$lp = 96
try {
    $lp = (Get-ItemProperty 'HKCU:\Control Panel\Desktop' -Name LogPixels -ErrorAction Stop).LogPixels
    if ($lp -gt 0) { $ratio = $lp / 96.0 }
} catch {
    Add-Line 'DPI 设置: 未设置（通常即 96 = 100%）'
}
Add-Line ('缩放: {0:0.##} 倍（LogPixels={1}，约 {2}%）' -f $ratio, $lp, [int](100 * $ratio))

if ($vw -gt 0 -and [Math]::Abs($ratio - 1.0) -gt 0.01) {
    $px = [int][Math]::Round($vw * $ratio)
    $py = [int][Math]::Round($vh * $ratio)
    Add-Line ('推算物理像素: {0}x{1}' -f $px, $py)
    Add-Line ''
    Add-Line '★ 注意：本脚本（powershell.exe / pwsh）不是 PerMonitorV2 感知进程，'
    Add-Line '   系统把坐标除以缩放比之后才给我们，所以上面"虚拟屏幕"是【逻辑坐标】。'
    Add-Line '   YZTrainer 清单声明了 PerMonitorV2，它读到的是【物理像素】。'
    Add-Line ('   两者差 {0:0.##} 倍 —— 现场若出现"整屏 BitBlt 尺寸判据失配"，先对这两个数。' -f $ratio)
} else {
    Add-Line '推算物理像素: 无需换算（100% 缩放）'
}

$text = ($lines -join "`r`n")
Write-Host ''
Write-Host '==== 屏幕几何基线 ===='
$lines | ForEach-Object { Write-Host ('  ' + $_) }
Write-Host ''
if ($Out -ne '') {
    try {
        $dir = Split-Path -Parent $Out
        if ($dir -ne '' -and -not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
        Set-Content -LiteralPath $Out -Value $text -Encoding utf8
        Write-Host ('  已写入: ' + $Out)
    } catch {
        Write-Host ('  写入失败: ' + $_.Exception.Message)
    }
}