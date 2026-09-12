<#
    构建 YZTrainer 解决方案（需要先安装 Visual Studio 的“使用 C++ 的桌面开发”工作负载）。

    用法：
        pwsh -File build.ps1                     # Release | Win32（机房学生端是 x86，默认就够）
        pwsh -File build.ps1 -Platform x64       # 只生成 64 位版本
        pwsh -File build.ps1 -Both               # Win32 与 x64 都生成
        pwsh -File build.ps1 -Configuration Debug

    产物统一输出到 dist\<Platform>\ ：
        YZTrainer.exe   主程序（需要管理员运行）
        YZHook.dll      注入模块（已内嵌进 exe，dist 下这份仅作回退/调试）
        YZProbe.exe     机房侦察工具
        YZSimTarget.exe 模拟远志学生端（仅本地测试用）
#>
param(
    [ValidateSet('Win32', 'x64')] [string]$Platform = 'Win32',
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release',
    [switch]$Both
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$sln  = Join-Path $root 'YZTrainer.sln'

if (-not (Test-Path $sln)) { throw "找不到解决方案文件: $sln" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw '找不到 vswhere.exe，说明本机还没有安装 Visual Studio。请用 VS Installer 勾选工作负载 “使用 C++ 的桌面开发”（Microsoft.VisualStudio.Workload.NativeDesktop），组件至少包含 Microsoft.VisualStudio.Component.VC.Tools.x86.x64 与任一 Windows11SDK。'
}

$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) {
    throw '找不到 MSBuild.exe：请在 VS Installer 中安装工作负载 “使用 C++ 的桌面开发”。'
}

Write-Host "MSBuild : $msbuild"

$platforms = if ($Both) { @('Win32', 'x64') } else { @($Platform) }

foreach ($p in $platforms) {
    Write-Host "==== 生成 $Configuration | $p ===="
    & $msbuild $sln /m /nologo /v:m /p:Configuration=$Configuration /p:Platform=$p
    if ($LASTEXITCODE -ne 0) {
        throw "构建失败: $Configuration|$p (exit=$LASTEXITCODE)"
    }
}

Write-Host ''
Write-Host '构建产物：'
Get-ChildItem (Join-Path $root 'dist') -Recurse -Include *.exe, *.dll -ErrorAction SilentlyContinue |
    Select-Object FullName, Length | Format-Table -AutoSize

Write-Host ''
Write-Host '提示：YZHook.dll 已内嵌进 YZTrainer.exe，单文件即可运行（运行时释放到 %ProgramData%\YZTrainer\cache）；YZProbe.exe 建议单独带到机房运行。'
