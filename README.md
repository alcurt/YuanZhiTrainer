# YZTrainer

针对 **广州远志 YZinfo 多媒体教学网络系统 V9.0 学生端** 的课堂辅助工具，参照 JiYuTrainer 的思路重新实现（不复制其代码）。

## 它做什么

* **广播窗口化**：教师端进行全屏广播时，把全屏、置顶、无边框的广播窗口改成可自由拖动缩放的普通窗口，画面与声音继续，你可以边看老师演示边操作自己的电脑。
* **解除键鼠锁定**：拦截学生端安装的 `WH_KEYBOARD_LL` / `WH_MOUSE_LL` / `WH_KEYBOARD` / `WH_GETMESSAGE` 等钩子，阻止其屏蔽 Alt+Tab、Win 键、任务管理器，并解除 `ClipCursor` 鼠标锁定、`BlockInput` 输入封锁与相关 HKCU 策略改写。
* **防监视（默认关闭）**：开启后教师端“监视/远程桌面”看到的是开启瞬间的静止画面（冻结帧），关闭立即恢复实时。
* **拦截教师端遥控输入（默认关闭）**：开启后丢弃学生端进程内的 `SendInput` / `mouse_event` / `keybd_event` / `SetCursorPos`，教师端无法接管你的鼠标键盘。
* **服务/驱动面板**：列出与远志相关的服务与内核驱动，只做临时停止/启动，不删除任何服务或文件。

## 它不做什么（硬边界）

* 不写内核驱动、不做手动映射、不碰远志的驱动文件。
* 不修改、不替换任何远志文件；不写持久化注册表项（只在本进程存活期间恢复被远志改写的 HKCU 策略值，退出时还原）。
* 不向其他机器发送指令、不做 UDP 攻击、不给同学机器远程发消息或执行命令。
* 不做免杀、混淆、反检测。
* **检测到考试/测验模式（加载 `Exam.ads`/`ExamDlg.exe`/`ClassQuiz.ads`/`ORAL_EXAM.ocx` 或出现考试类窗口标题）时，全部功能立即自动停用**，本工具不留绕过开关。
* 不尝试在无管理员权限的环境下工作。

## 工程结构

```
YZTrainer/
  YZTrainer.sln             VS 解决方案（4 个工程）
  build.ps1                 命令行构建脚本（调用 MSBuild）
  src/common/               公共协议、日志、工具函数
  src/YZTrainer/            主程序：托盘 UI、热键、注入器、命名管道服务、服务面板、诊断导出
  src/YZHook/               注入到远志学生端的 Hook DLL（MinHook + 窗口/输入/采集三层）
  src/YZProbe/              机房侦察工具（进程/模块/窗口/服务/网络快照，输出 JSON + Markdown）
  src/YZSimTarget/          模拟远志学生端（本地回归测试用）
  third_party/minhook/      MinHook（BSD-2-Clause，已内置源码）
  tests/run_sim_test.ps1    本地自动回归脚本
  dist/                     构建产物输出目录
```

## 工具链准备（只需一次）

Visual Studio 安装器勾选：

* 工作负载：`Microsoft.VisualStudio.Workload.NativeDesktop`（使用 C++ 的桌面开发）
* 组件：`Microsoft.VisualStudio.Component.VC.Tools.x86.x64`、任一 `Microsoft.VisualStudio.Component.Windows11SDK.*`

不需要 WDK，不需要 MFC，不需要 .NET 桌面运行时。

命令行等价操作（按需修改安装路径）：

```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vs_installer.exe" modify `
  --installPath "D:\VisualStudioApp" `
  --add Microsoft.VisualStudio.Workload.NativeDesktop `
  --includeRecommended --quiet --norestart
```

## 构建

```powershell
pwsh -File build.ps1              # Release | Win32（机房学生端是 x86，默认即可）
pwsh -File build.ps1 -Platform x64
pwsh -File build.ps1 -Both
```

也可以直接用 VS 打开 `YZTrainer.sln`，选择 `Release | Win32` 生成解决方案。产物在 `dist\Win32\`：

| 文件 | 用途 |
|---|---|
| `YZTrainer.exe` | 主程序（清单要求管理员权限），已内嵌 YZHook.dll，单文件即可运行 |
| `YZHook.dll` | 注入模块，运行时从 exe 资源释放；dist 下这份是回退/调试用，分发时不需要拷贝 |
| `YZProbe.exe` | 机房侦察工具，只读，建议先跑它 |
| `YZSimTarget.exe` | 模拟学生端，仅本地测试用 |

## 建议的使用流程

1. **本地先跑通**：管理员 PowerShell 执行 `pwsh -File tests\run_sim_test.ps1`，确认窗口化、解锁、冻结三项断言通过。
2. **机房侦察**：把 `YZProbe.exe`（x64 版本信息最全）拷到机房机器，右键以管理员运行，生成 `YZProbe-report-*.json/.md` 后拷回。重点看：
   * “远志相关进程”里哪个进程加载了 `Rmdesk.ads` / `PlayerGUI.dll` / `ExdHooks.dll`；
   * “疑似全屏/置顶窗口”里广播窗口的类名、样式与所属 PID；
   * “远志相关服务/驱动”的 ImagePath，确认文件过滤/网卡过滤驱动的真实名字。
3. **机房实测**：只需把 `YZTrainer.exe` 单文件拷过去，管理员运行。教师广播时观察广播窗口是否变成可操作窗口；退出程序后所有 hook 会被卸载、被改写的策略值会还原。
4. **出问题**：点“导出诊断包”，会把日志、配置、进程/窗口/服务快照打包到 `diag-<时间戳>\`，可带回分析。

## 界面与热键

| 热键 | 功能 |
|---|---|
| `Ctrl+Alt+F9` | 切换“广播窗口化” |
| `Ctrl+Alt+F10` | 切换“解除键鼠锁定” |
| `Ctrl+Alt+F11` | 切换“防监视（冻结画面）” |
| `Ctrl+Alt+F12` | 显示/隐藏主界面 |

配置文件 `YZTrainer.ini`（与 exe 同目录，目录不可写时仅内存生效）：

```ini
[General]
Flags=5            ; 1=窗口化 2=窗口置顶 4=键鼠解锁 8=防监视 16=拦遥控
WindowPercent=60   ; 窗口化后的宽度占屏幕百分比（20-100）
LogLevel=2         ; 0=错误 1=警告 2=信息 3=调试
AutoInject=1       ; 是否自动注入 / 客户端被服务重启后自动补注入
TargetDir=         ; 远志安装目录，留空=自动判定
HookDllPath=       ; 留空=用内嵌的 YZHook.dll；填路径可强制改用外部 DLL（调试用）
ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe
```

日志：`%TEMP%\YZTrainer\yzt.log`（超过 2MB 自动换用 `yzt-1.log`，不删除旧文件）。

## 实现要点

* **注入**：主程序启用 `SeDebugPrivilege`，`CreateRemoteThread` + `LoadLibraryW` 把内嵌在 exe 资源里的 `YZHook.dll` 注入目标进程（释放到 `%ProgramData%\YZTrainer\cache\`、按内容哈希命名、先校验 PE 架构）；目标按“进程名 + 路径含 GYZY/YZinfo + 加载了 `.ads`/`PlayerGUI.dll`/`ExdHooks.dll`”打分选取；每 2 秒巡检，客户端被服务重启后自动补注入。
* **窗口层**：hook `SetWindowPos` / `MoveWindow` / `ShowWindow` / `SetWindowLongA/W`，命中“本进程 + 无标题栏 + 覆盖整块显示器 + 置顶或 POPUP”的窗口后改写成 `WS_OVERLAPPEDWINDOW`，默认屏宽 60% 居中、不抢焦点；客户端每 3 秒抢回全屏时按 500ms 节流重新纠正。
* **输入层**：拦截学生端安装键盘钩子的调用；`RegisterHotKey`、`SystemParametersInfoW`（屏保/快速任务切换）、`ClipCursor`、`BlockInput` 全部按开关放行或拦截；每秒强制 `ClipCursor(NULL)` 一次；被改写的 HKCU 策略值（任务管理器/锁屏/Win 键/GameDVR）在后台持续恢复，退出时还原原值。
* **采集层**：跟踪进程内取得的屏幕 DC（`GetDC(NULL)`/`GetWindowDC`/`CreateDCW("DISPLAY")`），冻结时把 `BitBlt`/`StretchBlt`/`PrintWindow` 的源改为开启瞬间抓取的 DIB，从而只影响学生端自己抓屏，不影响你本地显示。
* **通信**：命名管道 `\\.\pipe\YZTrainer`，帧格式 `[magic 'YZT1'][opcode][len][payload]`；Hook DLL 主动连接并上报状态与日志。
* **还原**：收到卸载指令或客户端进程退出时，全量 `MH_DisableHook` + `MH_RemoveHook` + `MH_Uninitialize`，恢复策略值，释放冻结位图；不调用 `FreeLibrary`（避免崩溃），DLL 留在进程内但完全停用。

## 已知限制

1. 防监视目前覆盖 GDI 抓屏路径。若侦察报告显示学生端改用 `ExdDtDup.dll`（DXGI Desktop Duplication）抓屏，需要在 `hooks_capture.cpp` 追加 `IDXGIOutputDuplication::AcquireNextFrame` 的 COM vtable hook（计划中的下一步）。
2. 若键鼠封锁来自内核驱动或 `WinIo.dll` 端口 I/O（即解除钩子后仍被锁），需要改用“指令层拦截”：在 `CmdProc.ads`/`NetControl.ads` 的分发点丢弃锁定类命令，命令标识需先用侦察报告与抓包确认。
3. 注入需要管理员权限与足够完整性级别；若学生端以 SYSTEM 运行而你在受限账户下，会失败（机房默认管理员登录则不受影响）。
4. 载荷已内嵌（RCDATA）：运行时释放到 `%ProgramData%\YZTrainer\cache\YZHook_<arch>_<hash>.dll`，复用前整文件比对；目录不可写时退 `%TEMP%\YZTrainer-cache\`，再失败才回退 exe 同目录的 `YZHook.dll`。释放出来的文件保留不删，便于排障。
5. 防监视从设计上只影响学生端自己抓屏的路径，不会改动系统显示驱动。

## 许可与声明

本项目仅用于个人学习与在**不影响正常教学**的前提下改善课堂体验（作者立场与 JiYuTrainer 相同）。
请勿在考试、测验、监考等场景使用；工具已内置考试模式自动停用。
第三方组件：MinHook（BSD-2-Clause，见 `third_party/minhook/LICENSE.txt`）。
