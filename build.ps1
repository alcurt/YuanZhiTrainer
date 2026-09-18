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

# dist 里始终放一份“出厂默认”INI：避免把调试用的配置（例如 Flags=13 打开防监视）
# 随压缩包发给别人。内容保持纯 ASCII，GetPrivateProfileStringW 读取才不会有编码问题。
foreach ($p in $platforms) {
    $distDir = Join-Path $root "dist\$p"
    if (-not (Test-Path $distDir)) { continue }
    $ini = Join-Path $distDir 'YZTrainer.ini'
    $default = @(
        '[General]',
        'Flags=5',
        'WindowPercent=60',
        'LogLevel=2',
        'AutoInject=1',
        'InjectMethod=0',
        'EnableExamGuard=1',
        'TargetDir=',
        'HookDllPath=',
        'ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe'
    )
    Set-Content -LiteralPath $ini -Value $default -Encoding utf8
    Write-Host "已写入默认配置: $ini"
}

Write-Host ''
Write-Host '提示：YZHook.dll 已内嵌进 YZTrainer.exe，单文件即可运行（运行时释放到 %ProgramData%\YZTrainer\cache）；YZProbe.exe 建议单独带到机房运行。'
