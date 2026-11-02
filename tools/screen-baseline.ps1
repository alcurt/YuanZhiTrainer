# screen-baseline.ps1 —— 现场屏幕几何基线（一次性打印，供现场备注引用）
#
# 为什么需要：厂商抓屏与我们的替换都按"虚拟屏幕矩形"算（SM_XVIRTUALSCREEN 那一组）。
# 多显示器、扩展模式、DisplayFusion 这类工具都会让虚拟屏幕与主显示器不一致，
# 一旦对不上，"冻不上"到底是几何问题还是逻辑问题，事后从日志里分不出来。
#
# 用法（两个版本 PowerShell 都行，不需要管理员）：
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\screen-baseline.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\screen-baseline.ps1 -Out D:\带回\屏幕基线.txt
#
# 注意：不要写成 `powershell -Command "… $_ …"` 那种嵌套单行——外层会把 $_ 展开成空，
#       报"必须在 -f 运算符后面提供一个值表达式"。这个坑在 PowerShell 5.1 与 7.x 上都存在。

param([string]$Out = '')

$lines = New-Object System.Collections.Generic.List[string]
function Add-Line([string]$s) { [void]$script:lines.Add($s) }

Add-Line ('时间: ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Add-Line ('主机: ' + $env:COMPUTERNAME + '   用户: ' + $env:USERNAME)
Add-Line ('PowerShell: ' + $PSVersionTable.PSVersion.ToString() + '   64位=' + [Environment]::Is64BitProcess)

$sig = '[DllImport("user32.dll")] public static extern int GetSystemMetrics(int n);'
try {
    Add-Type -Namespace YzScr -Name Metrics -MemberDefinition $sig -ErrorAction Stop
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

try {
    $lp = (Get-ItemProperty 'HKCU:\Control Panel\Desktop' -Name LogPixels -ErrorAction Stop).LogPixels
    Add-Line ('DPI(LogPixels): {0} 约 {1}%' -f $lp, [int](100 * $lp / 96))
} catch {
    Add-Line 'DPI(LogPixels): 未设置（通常即 96 = 100%）'
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