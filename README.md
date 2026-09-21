# YZTrainer

> **v0.4.0** · 一款**远志多媒体教学管理软件 V9.0 网管版学生端**的解控软件 · 仅供**教育研究**与**技术学习**，禁止任何非法用途 · 与厂商无关联，使用者自行承担全部后果。

针对 **广州远志（Howyar）YZinfo 多媒体教学网络系统 V9.0 网管版学生端** 的课堂辅助工具（普通学生端同样适用，实测机房部署的是网管版），参照 JiYuTrainer 的思路重新实现（不复制其代码）。

## 它做什么

* **广播窗口化**：教师端进行全屏广播时，把全屏、置顶、无边框的广播窗口改成可自由拖动缩放的普通窗口，画面与声音继续，你可以边看老师演示边操作自己的电脑。**注入进不去也能用**：进程内 Hook 拿不到目标进程时，改由主程序跨进程改窗口样式（`ExternalWindowFix`，默认开）。
* **解除键鼠锁定**：拦截学生端安装的 `WH_KEYBOARD_LL` / `WH_MOUSE_LL` / `WH_KEYBOARD` / `WH_GETMESSAGE` 等钩子，阻止其屏蔽 Alt+Tab、Win 键、任务管理器，并解除 `ClipCursor` 鼠标锁定、`BlockInput` 输入封锁与相关 HKCU 策略改写。只拦**确实是远志自己装的**钩子与**逃生类组合键**，不动同进程里其它组件的合法钩子/热键。
* **防监视（默认关闭）**：开启后教师端“监视/远程桌面”看到的是开启瞬间的静止画面（冻结帧），关闭立即恢复实时。
* **拦截教师端遥控输入（默认关闭）**：开启后丢弃学生端进程内的 `SendInput` / `mouse_event` / `keybd_event` / `SetCursorPos`，教师端无法接管你的鼠标键盘。
* **服务/驱动面板**：列出与远志相关的服务与内核驱动，只做临时停止/启动，不删除任何服务或文件。

## 它不做什么（硬边界）

* 不写内核驱动、不做手动映射、不碰远志的驱动文件。
* 不修改、不替换任何远志文件；不写持久化注册表项（只在本进程存活期间恢复被远志改写的 HKCU 策略值，退出时还原）。
* 不向其他机器发送指令、不做 UDP 攻击、不给同学机器远程发消息或执行命令。
* 不做免杀、混淆、反检测。
* **检测到考试/测验的强信号时，全部功能立即自动停用**：独立考试进程 `ExamDlg.exe`、进程内加载 `ExamDlg.exe` / `ExamDlg.dll` / `ORAL_EXAM.ocx` / `ExamEditor.exe`，或存在**可见**且标题命中考试类关键词的远志窗口。注意 `Exam.ads` / `ClassQuiz.ads` 属学生端启动即加载的常驻模块（弱信号），**只会记日志、不再触发熔断**——它们是就绪态而非"正在考试"的判据。
* 不尝试在无管理员权限的环境下工作。

## 工程结构

```
YZTrainer/
  YZTrainer.sln             VS 解决方案（5 个工程）
  build.ps1                 命令行构建脚本（调用 MSBuild）
  src/common/               公共协议、日志、工具函数
  src/YZTrainer/            主程序：托盘 UI、热键、注入器、命名管道服务、服务面板、诊断导出
  src/YZHook/               注入到远志学生端的 Hook DLL（MinHook + 窗口/输入/采集三层）
  src/YZProbe/              机房侦察工具（进程/模块/窗口/服务/网络快照，输出 JSON + Markdown）
  src/YZSimTarget/          模拟远志学生端（本地回归测试用）
  src/YZUnhookTest/         免注入拔钩验证工具（本进程加载远志 DLL 调其导出，不注入）
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

也可以直接用 VS 打开 `YZTrainer.sln`，选择 `Release | Win32` 生成解决方案。四个工程统一钉在 `WindowsTargetPlatformVersion 10.0.22621.0`（与机房 Win11 23H2 对齐，需装该版本或更高的 Windows SDK）；主程序清单同时声明 `requireAdministrator` 与 `PerMonitorV2` 高 DPI（换显示器会重排界面，125%/150%/175% 下都清晰）。产物在 `dist\Win32\`：

| 文件 | 用途 |
|---|---|
| `YZTrainer.exe` | 主程序（清单要求管理员权限），已内嵌 YZHook.dll，单文件即可运行 |
| `YZHook.dll` | 注入模块，运行时从 exe 资源释放；dist 下这份是回退/调试用，分发时不需要拷贝 |
| `YZProbe.exe` | 机房侦察工具，只读，建议先跑它 |
| `YZUnhookTest.exe` | 免注入拔钩验证工具：默认只体检（读文件导出表 + 调用约定自检）并把过程写入同目录 `YZUnhookTest-<时间戳>.txt`；`--apply` 才真正调用远志的拔钩导出，`--arm` 待机等你回车再执行，`--delay N` 待机 N 秒后自动执行（被锁键鼠时唯一可行的触发方式），`--no-pause` 供脚本调用 |
| `YZSimTarget.exe` | 模拟学生端，仅本地测试用 |

## 建议的使用流程

1. **本地先跑通**：管理员 PowerShell 执行 `pwsh -File tests\run_sim_test.ps1`，确认两阶段断言全部通过——阶段一（注入路径）四项：全屏置顶 / 窗口化 / 带 WS_CAPTION / 冻结画面；阶段二（免注入路径）两项：模拟目标仍被窗口化、外部纠正计数大于 0。
2. **机房侦察（先跑探针）**：把 `YZProbe.exe`（x64 版本信息最全）拷到机房机器，右键以管理员运行，生成 `YZProbe-report-*.json/.md`。重点看：
   * “远志相关进程”里哪个进程加载了 `Rmdesk.ads` / `PlayerGUI.dll` / `ExdHooks.dll`；
   * 远志模块导出表里 `KillHook` / `UnSetExdHooks` / `UnSetExdHooks2` 是否存在（与本地版本比对，决定主动拔钩可用性）；
   * “疑似全屏/置顶窗口”里广播窗口的类名、样式与所属 PID（确认是否由 `ExdPaintHelper.exe` 承载）；
   * “远志相关服务/驱动”的 ImagePath，确认文件过滤/网卡过滤驱动的真实名字；
   * 进程模块里是否出现 `ExdDtDup.dll`（决定防监视能不能用）；`self` 段的完整性级别/提权/SeDebug。
   * **尽量在教师正在广播时再采一份**：只有广播进行中才会出现广播窗口，"疑似全屏/置顶窗口"表里的那行（类名/样式/所属 PID）是判断窗口化判据够不够用的唯一硬证据。2026-09-18 的两份报告都是无人广播时采集的，这一点至今仍是空白。
3. **报告留在 `YZTrainer.exe` 同目录**：诊断包会自动把它一起打包，省得漏带。
4. **机房实测（再跑主程序）**：把 `YZTrainer.exe` 单文件拷过去，**先确认没有别的 YZTrainer 在跑**（全局互斥体，第二份会静默退出），管理员运行，日志里按顺序确认三件事：
   * `目标进程 pid=… (…) 命中: …`（应优先命中“广播窗口宿主”）；
   * `已注入目标进程 pid=…` 与 Hook 侧的 `Hook 安装完成，共 23 个`；
   * `NativeUnhook: 已调用 …`（远志模块已加载时）——这是“能否解掉已经锁死的机器”的直接证据。
   随后让教师开始广播，看广播窗口是否变成可操作窗口、键鼠是否可用；`Ctrl+Alt+F9/F10/F11/F12` 是对应开关，注意**防监视默认关，只在明确要演示冻结画面时按**。
   如果日志里只有 `注入失败: VirtualAllocEx 失败 …（拒绝访问 …）`，请把 `InjectMethod` 改成 `1` 再试消息钩子；两条路都不通时，窗口化仍由外部纠正负责，日志里会出现 `外部窗口纠正: 0x… -> x,y wxh`，状态栏“窗口化次数”的第二项就是它的计数。
5. **导出诊断包**：它是**即时快照**，在“你希望被记录的那个状态”下导出。建议导两份——一份正常基线、一份出问题的那一刻；只能带一份回来时优先带异常那份。输出在 exe 同目录 `diag-<时间戳>\`，含日志（含滚动档）、`snapshot.txt`（配置/运行状态/进程/目标窗口/服务）与同目录的 `YZProbe-report-*`。退出程序后确认 hook 已卸载、被改写的策略值已还原。

## 界面与热键

| 热键 | 功能 |
|---|---|
| `Ctrl+Alt+F9` | 切换“广播窗口化” |
| `Ctrl+Alt+F10` | 切换“解除键鼠锁定” |
| `Ctrl+Alt+F11` | 切换“防监视（冻结画面）” |
| `Ctrl+Alt+F12` | 显示/隐藏主界面 |

其中**窗口化、解除键鼠锁定默认开；防监视、拦截遥控默认关**（即默认 `Flags=5`）。托盘右键菜单有前三项开关与“立即注入”的等价入口，热键被其它程序占用时用菜单。

托盘交互：**双击托盘图标显示主界面**（与隐藏时的气泡提示一致），单击/右键打开菜单；第一次把窗口关掉或最小化时，会弹一条气泡提示“窗口已隐藏到托盘”，避免误以为程序退出了。

配置文件 `YZTrainer.ini`（与 exe 同目录，目录不可写时仅内存生效）：

```ini
[General]
Flags=5            ; 1=窗口化 2=窗口置顶 4=键鼠解锁 8=防监视 16=拦遥控
WindowPercent=60   ; 窗口化后的宽度占屏幕百分比（20-100）
LogLevel=2         ; 0=错误 1=警告 2=信息 3=调试
AutoInject=1       ; 是否自动注入 / 客户端被服务重启后自动补注入
InjectMethod=0     ; 注入方式：0=CreateRemoteThread+LoadLibrary（默认）；1=SetWindowsHookEx 消息钩子
TargetDir=         ; 远志安装目录，留空=自动判定
HookDllPath=       ; 留空=用内嵌的 YZHook.dll；填路径可强制改用外部 DLL（调试用）
EnableExamGuard=1  ; 考试模式守护：1=仅强信号熔断（默认）；0=完全跳过考试检测
ExternalWindowFix=1; 免注入的外部窗口纠正：1=开（默认）0=关，只在窗口化开关打开时生效
ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe
```

日志：`%TEMP%\YZTrainer\yzt.log`（超过 2MB 自动换用 `yzt-1.log`，不删除旧文件）。

## 实现要点

* **注入**：主程序启用 `SeDebugPrivilege`，`CreateRemoteThread` + `LoadLibraryW` 把内嵌在 exe 资源里的 `YZHook.dll` 注入目标进程（释放到 `%ProgramData%\YZTrainer\cache\`、按内容哈希命名、先校验 PE 架构）；每 2 秒巡检，客户端被服务重启后自动补注入。
* **目标选择（三路打分 + 一路兜底）**：① 正在全屏广播的窗口宿主 PID，**+5**，判据与 Hook 侧完全一致（无属主 / 无标题栏 / 非子窗口 / 非桌面壳类 / 覆盖整块显示器 / POPUP 或置顶）；② 进程名在 `ProcessNames` 名单，**+3**；③ 安装路径含 `YZinfo Multimedia teaching software` 或 `GYZY`，**+2**。三者都没命中时，才逐个查进程模块是否加载了 `Rmdesk.ads` / `PlayerGUI.dll` / `ExdHooks.dll`（代价高，仅兜底）。选中后在日志里写明命中原因。
* **主动拔钩**：用户态没有受支持的 API 能摘掉别人装的钩子，但远志自己导出了卸载入口——解锁开关生效时调用 `KeyboardHook.dll!KillHook()`、`ExdHooks.dll!UnSetExdHooks(0)`、`ExdHooks.dll!UnSetExdHooks2(GetCurrentThreadId())`，拔掉“注入之前”就已经装好的全局/线程钩子。调用放在独立工作线程并限时等待（`KillHook` 结尾是 `WaitForSingleObject(..., INFINITE)`，直接在引擎线程调用有挂死风险），调用点用 SEH 兜底。
* **调用约定自检（fail-closed）**：调用远志自带导出前，用内置的 HDE32 走真实指令流确认函数以 `ret` / `ret 0` 收尾、单参函数确实读取 `[esp+4]`；出现 `ret imm16 != 0`、远返回、thunk(`jmp`) 或解析失败一律拒绝调用并记 ERROR。裸字节扫描会被指令里的 ModRM 字节骗到（例如 `3B C3` 里的 `0xC3`），所以必须走解码。
* **备用注入方式（消息钩子）**：`InjectMethod=1` 时，主程序用 `SetWindowsHookEx(WH_GETMESSAGE)` 把已释放的 `YZHook.dll` 挂到会话的 GUI 线程上，由系统加载器把 DLL 映射进目标进程——我们自己不写目标内存，因此能绕开"内核组件剥夺句柄 VM 权限"这一类保护（`OpenProcess` 成功、`VirtualAllocEx` 返回 0x5 的场景）。代价是 DLL 会被映射进本会话所有 GUI 进程（宿主识别不通过的进程里只是空转），因此默认仍用远程线程方式，只在被剥夺句柄权限的机器上切到 1。
* **注入能力诊断（YZProbe）**：探针用 `GetProcessInformation(ProcessProtectionLevelInfo)` 读进程保护级别（`PROTECTION_LEVEL` 枚举，`0xFFFFFFFE`=无保护）、用 `NtQueryObject` 读句柄**实得权限**，再逐项实测模块枚举 / `ReadProcessMemory` / `VirtualAllocEx` / `WriteProcessMemory`（只写自己刚分配的一页并立刻释放）。`--access <pid>` 可对单个进程单独诊断。它同时列出非微软签名的内核驱动（保护件/杀软/还原卡）与服务的 `SERVICE_CONFIG_LAUNCH_PROTECTED` 标志。
* **免注入拔钩（YZUnhookTest）**：`KeyboardHook.dll` 的 `HookData` 与 `ExdHooks.dll` 的 `.ExdHook` 都是跨进程共享节，HHOOK 又是会话级对象——因此在**本进程**里加载这两个 DLL 并调用 `KillHook()` / `UnSetExdHooks(0)` / `UnSetExdHooks2(GetCurrentThreadId())`，撤销的是同一个钩子集合，完全不需要注入。工具默认只体检（读文件导出表 + 调用约定自检），`--apply` 才真调用，并在调用前后打印共享节里的句柄/状态字作为对照。
* **窗口层**：hook `SetWindowPos` / `MoveWindow` / `ShowWindow` / `SetWindowLongA/W`，命中“本进程 + 无标题栏 + 覆盖整块显示器 + 置顶或 POPUP”的窗口后改写成 `WS_OVERLAPPEDWINDOW`，默认屏宽 60% 居中、不抢焦点；客户端每 3 秒抢回全屏时按 500ms 节流重新纠正。
* **外部窗口纠正（免注入兜底）**：窗口样式是会话级对象，改别人的窗口不需要目标进程配合。主程序每秒按**同一套结构判据**枚举顶层窗口，命中"属于远志进程 + 无属主 + 无标题栏 + 非子窗口 + 非桌面壳类 + 覆盖整块显示器 + POPUP 或置顶"后用跨进程 `SetWindowLongPtrW` + `SetWindowPos(..., SWP_ASYNCWINDOWPOS)` 改成普通窗口，节流 1 秒。这是"客户端被剥夺句柄权限、`VirtualAllocEx` 返回 0x5"时唯一还能生效的窗口化手段；进程内 Hook 已生效的宿主进程会被跳过（避免两边抢同一个窗口），其余远志进程仍由它兜底。写入被拒（UIPI / win32k 权限检查）会记 WARN 并在状态栏显示"有写入失败"，不静默失败。
* **输入层**：拦截学生端安装键盘钩子的调用——**线程内钩子（`hMod == NULL`）按回调地址反查所属模块**，只有落在远志目录里才拦，归属不明的放行并记账（避免误伤输入法/公共控件）；`RegisterHotKey` 只拦 Win / Alt+Tab / Alt+Esc / Alt+F4 / Alt+Space / Ctrl+Esc / Ctrl+Shift+Esc 这些"逃生键"；`SystemParametersInfoW`（屏保/快速任务切换）、`ClipCursor`、`BlockInput` 按开关放行或拦截；每秒强制 `ClipCursor(NULL)` 一次；被改写的 HKCU 策略值（任务管理器/锁屏/Win 键/GameDVR）在后台持续恢复，退出时还原原值。拦截遥控输入时 `SendInput` 返回"全部投递成功"而不是 0，减少被学生端察觉后重试的概率。
* **采集层**：跟踪进程内取得的屏幕 DC（`GetDC(NULL)`/`GetWindowDC`/`CreateDCW("DISPLAY")`），冻结时把 `BitBlt`/`StretchBlt`/`PrintWindow` 的源改为开启瞬间抓取的 DIB，从而只影响学生端自己抓屏，不影响你本地显示。
* **通信**：命名管道 `\\.\pipe\YZTrainer`，帧格式 `[magic 'YZT1'][opcode][len][payload]`；Hook DLL 主动连接并上报状态与日志。
* **考试模式判定（强/弱信号分级）**：弱信号（`Exam.ads` / `ClassQuiz.ads` 已加载）只按节流记 INFO；强信号（独立 `ExamDlg.exe` 进程、进程内加载考试对话框组件、或**可见**且标题命中 `考试|测验|答题|试卷|快问快答|Exam|Quiz` 的远志窗口）任一命中才熔断并恢复策略、清零功能位。判据走严格目录前缀比较（避免 `...V9.0 StudentX` 这类前缀误判），窗口用 `IsWindowVisible` 过滤隐藏窗口；熔断时输出诊断（命中原因 / 考试进程路径 / 强信号模块 / 进程模块清单）到日志与 `%TEMP%\YZTrainer\exam-debug.txt`，同内容 30 秒节流 + FNV-1a 去重。开关 `EnableExamGuard`（默认 1）以控制位 `YZ_CFG_EXAM_GUARD` 随配置下发，用 `YZ_FLAG_FUNCTION_MASK` 剔除，保证功能位上报不被污染。
* **还原**：收到卸载指令或客户端进程退出时，全量 `MH_DisableHook` + `MH_RemoveHook` + `MH_Uninitialize`，恢复策略值，释放冻结位图；不调用 `FreeLibrary`（避免崩溃），DLL 留在进程内但完全停用。

## 已知限制

1. 防监视目前只覆盖 GDI 抓屏路径。对 V9.0 Student 安装目录做静态扫描已确认存在 `KsFiles\ExdDtDup.dll`（DXGI Desktop Duplication），广播/监视的抓屏很可能走 DXGI 路径，GDI 冻结未必拦得住——所以**防监视保持默认关闭**，要用之前先在机房实测。要覆盖该路径需在 `hooks_capture.cpp` 追加 `IDXGIOutputDuplication::AcquireNextFrame` 的 COM vtable hook（尚未实现）。
2. 若键鼠封锁来自内核驱动或 `WinIo.dll` 端口 I/O（即解除钩子后仍被锁），需要改用“指令层拦截”：在 `CmdProc.ads`/`NetControl.ads` 的分发点丢弃锁定类命令，命令标识需先用侦察报告与抓包确认。
3. 注入需要管理员权限与足够完整性级别；若学生端以 SYSTEM 运行而你在受限账户下，会失败（机房默认管理员登录则不受影响）。
4. 载荷已内嵌（RCDATA）：运行时释放到 `%ProgramData%\YZTrainer\cache\YZHook_<arch>_<hash>.dll`，复用前整文件比对；目录不可写时退 `%TEMP%\YZTrainer-cache\`，再失败才回退 exe 同目录的 `YZHook.dll`。释放出来的文件保留不删，便于排障。
5. 防监视从设计上只影响学生端自己抓屏的路径，不会改动系统显示驱动。
6. **同一时间只能运行一份**：主程序用全局互斥体 `Global\YZTrainer_SingleInstance` 防重入，第二份会弹提示后退出。若另一份是**非提权**的旧副本，它还会因完整性级别不够而 `OpenProcess` 失败（错误 0x5），表现就是“什么都没发生”——现场先确认没有其它 YZTrainer 在跑（含 `YZTrainer-noelev.exe` 这类改名副本）。
7. 主动拔钩依赖远志 DLL 已加载且导出签名与本地分析一致：签名不符时会被运行时自检拒绝（记 ERROR、功能不生效，但不影响其它功能）。本机 V9.0 Student 的 `KillHook` 为无参，`UnSetExdHooks` / `UnSetExdHooks2` 各带 1 个 DWORD 参数（与最初计划书“全部无参”的假设不同，实测内部调用点分别传 `0` 与调用者自身 tid）。
8. 外部窗口纠正虽然不需要注入，但**跨进程改样式仍要过 win32k 的权限检查**：机房实测 `Yistart.exe` 以 SYSTEM（完整性 16384）运行，而 YZTrainer 以管理员（高完整性 12288）运行，两者相差一个级别。若目标窗口的样式写入被拒，日志会记 WARN、状态栏显示“有写入失败”——这条路径是否可用以机房实测为准，不要只看本地模拟目标（模拟目标与主程序同完整性，必然成功）。
9. 外部窗口纠正依赖与注入器相同的结构判据，因此**广播窗口若带标题栏（`WS_CAPTION`）或既非 POPUP 也非置顶，同样会漏判**；2026-09-18 的探针报告是在无人广播时采集的，至今没有拿到"正在广播时"的窗口类名/样式证据（见下节）。

## 网管版（GZYZ）适配

机房里实测部署的是**网管版**，与普通学生端有几处差异，代码里已按下面规则处理：

* **安装前缀**：网管版默认装在 `C:\Program Files (x86)\GZYZ\YZinfo Multimedia teaching softwareV9.0 Student\`。所有"是否远志"的路径判据统一走 `yz::IsYuanzhiInstallPath()`，同时认 `YZinfo Multimedia teaching software` 与 `GZYZ` 两种前缀——注入器的目标打分、Hook 侧的宿主识别、考试模式的窗口归属判据都改用它，避免只认一种前缀造成漏判。探针的服务/模块过滤关键词本来就含 `GZYZ`。
* **守卫进程 Nmdeputy.exe**：服务以 `Nmdeputy.exe /NormalApp /GlobalMutex /StopParam:stop /Delay:1000 /Delay2:3000 /DelayInit:3000 /SelfGuard /Protection /Name:… Service /"…\Yistart.exe" -AutoUninstall` 的形式拉起客户端。它是**守护与上报**进程，因此：**注入器显式排除它**（`IsGuardProcess`，日志里提示一次"检测到网管版守卫进程"，只注入客户端）；同时正因为它会重启/重新保护客户端，看门狗每 2 秒补注入这条机制在网管版上是必需的，不能省。
* **目标优先级**：广播窗口宿主(+5) ＞ 主进程优先（`Yistart.exe` +3、`ExdPaintHelper.exe` +2、`TEACHCMD.exe`/`PlayerGUI.exe` +1）＞ 进程名在名单(+3) ＞ 安装路径匹配(+2)，守卫进程永不入选。这样即便名单里同时有多个客户端进程，也会稳定落到 `Yistart.exe` 或真正的广播窗口宿主上。
* **客户端被定向剥夺句柄权限（2026-09-18 两台机房机器实测）**：`Yistart.exe` 的进程保护级别是**无保护**（`0xFFFFFFFE`，即不是 PPL），而 `Nmdeputy.exe` 可以正常枚举模块、`VirtualAllocEx`/`WriteProcessMemory` 全部成功——说明保护是**只针对客户端进程**的句柄权限剥夺（`ObRegisterCallbacks` 一类），不是进程保护机制。典型现象：注入时报 `VirtualAllocEx 失败 0x00000005`，连模块都枚举不到。首选免注入路线（`YZUnhookTest`），或把 `InjectMethod` 切成 1 试消息钩子。
* **窗口化不依赖注入（v0.4.0 起）**：正因为上面这条，**广播窗口化改走双路**——进程内 Hook 拿不到目标进程时，主程序用跨进程 `SetWindowLongPtrW`/`SetWindowPos` 直接改那个窗口（`ExternalWindowFix=1` 默认开）。跨进程改样式是会话级操作，不需要目标进程配合，但需要过 win32k 的完整性检查（目标以 SYSTEM 运行、我们以管理员运行时是否放行，以机房实测为准，日志会记 WARN）。
* **首要怀疑对象**：`BrDevfer`（`system32\Drivers\BRUsbFilter.sys`）——驱动清单里它挂在“Windows (R) Win 7 DDK provider”这个通用厂商字符串下（用 DDK 自建驱动常见），名字像 USB 过滤，但完全可能同时注册进程回调。下次上机把它的文件版本与签名一并记下来。
* **打包提醒**：`build.ps1` 会在 `dist\<平台>\` 写入一份出厂默认 `YZTrainer.ini`（`Flags=5`、`LogLevel=2`、`EnableExamGuard=1`、`ExternalWindowFix=1`、`InjectMethod=0`），避免把调试配置（例如打开防监视的 `Flags=13`）随压缩包发出去。

### 为什么注入会失败（`VirtualAllocEx` 返回 0x5）

常规注入是四步，我们卡在第二步：

1. `OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | ...)` 拿到目标进程句柄；
2. **`VirtualAllocEx`**——在**目标进程的地址空间**里申请一块内存（我们要用它存 DLL 的路径字符串）；
3. `WriteProcessMemory` 把 DLL 路径写进那块内存；
4. `CreateRemoteThread(LoadLibraryW, 那块内存)` 让目标进程自己把 DLL 加载进去。

`VirtualAllocEx` 需要句柄带 `PROCESS_VM_OPERATION`。现在的情况是：`OpenProcess` **成功**（DACL 检查通过、`SeDebugPrivilege` 也开了），但拿到的句柄其 `GrantedAccess` 被**事后削减**——驱动用 `ObRegisterCallbacks` 注册进程回调，在每次有人打开受保护进程的句柄时剥掉 `PROCESS_VM_OPERATION`/`PROCESS_VM_WRITE`/`PROCESS_VM_READ`（可能还有 `PROCESS_CREATE_THREAD`）。于是第二步在对象管理器里被判 `STATUS_ACCESS_DENIED`，`GetLastError()` 得到 5。同一个原因也解释了为什么连模块枚举（内部要 `PROCESS_VM_READ`）都失败。

这**不是权限不够**：已经是管理员 + `SeDebugPrivilege`，提权再多也没用（`SeDebug` 绕的是 DACL，不绕句柄权限削减）。可走的路只有两条：不写目标内存（`InjectMethod=1` 消息钩子，由系统加载器映射 DLL）或干脆不注入（`YZUnhookTest` 在本进程调用远志自己的拔钩导出）。

## 免责声明与使用边界

本项目仅供**教育研究**与**技术学习**目的使用，为学习、研究相关软件在 Windows 用户态的运行机制（进程注入、API Hook、窗口与输入子系统）而编写，**禁止用于任何非法用途**。作者的出发点与 JiYuTrainer 相同：在**不影响正常教学**的前提下改善个人课堂体验。

* **与厂商无关联**：本项目由个人独立开发，与广州远志公司及其关联方无任何关系，未获其授权、认可或赞助；文中出现的产品名与商标归各自权利人所有，此处仅作指称性说明。
* **使用者自负其责**：你必须自行确保使用行为符合所在地法律法规、所在学校规章制度以及相关软件许可协议。因使用本软件导致的系统故障、数据丢失、账号或设备被封禁、纪律处分及其他任何后果，概由使用者自行承担，作者不承担任何责任。
* **禁止的使用场景**：严禁用于考试、测验、监考等任何评估环节；严禁破坏教学秩序、侵犯他人隐私、干扰他人设备；严禁任何攻击、牟利或二次分发牟利行为。规避技术措施与破坏计算机信息系统的行为，可能触犯《著作权法》相关条款及《刑法》第 285、286 条。
* **不规避技术措施**：本项目不以破解、复制、传播他人软件为目的，仓库内不包含任何厂商文件；请自行通过合法渠道获取相关软件。
* **无担保与已知风险**：软件按“现状”提供，不附带任何明示或默示担保。它需要管理员权限并向其他进程注入代码，可能被防病毒软件、EDR 或学校终端管理系统拦截、隔离或告警。
* **禁止恶意用途**：禁止将本项目或其衍生代码用于免杀、反检测、捆绑传播或任何攻击性用途。
* **考试模式硬边界**：程序检测到考试/测验的**强信号**时会立即自动停用全部功能并恢复被改写的策略值；默认开启的"考试模式守护"（`EnableExamGuard`）只做判定，**关闭它等于停止检测，不改变本项目的使用边界**——任何考试、测验、监考场景都禁止使用本工具。

本声明随版本更新；它不构成对使用者的责任豁免，也不能替代你的法律意见。

## Disclaimer (English)

This project is provided solely for **educational research** and **technical study**, in order to learn how Windows user-mode mechanisms such as process injection, API hooking, window management and the input subsystem work. It must not be used for any illegal purpose.

* **No affiliation.** This project is developed independently by an individual. It is not affiliated with, authorized by, endorsed by, or sponsored by Guangzhou Yuanzhi (YZinfo) or any of its affiliates. Product names and trademarks mentioned here belong to their respective owners and are used for identification purposes only.
* **You are responsible.** You must ensure that your use complies with all applicable laws, your school's regulations, and any relevant software license agreements. The author accepts no responsibility or liability for any system failure, data loss, account or device ban, disciplinary action, or any other consequence arising from the use of this software.
* **Prohibited uses.** Do not use this project during exams, quizzes, or any other form of assessment; do not use it to disrupt teaching, invade privacy, or interfere with other people's devices; do not use it for attacks or for profit. Circumventing technological protection measures or damaging computer information systems may violate copyright and criminal law in your jurisdiction.
* **No circumvention of protection measures.** This project is not intended to crack, copy, or redistribute anyone else's software, and this repository contains no vendor files. Obtain any third-party software through legitimate channels.
* **No warranty.** The software is provided "as is", without warranty of any kind. It requires administrator privileges and injects code into other processes, and may therefore be flagged, blocked, or quarantined by antivirus software, EDR, or your school's endpoint management system.
* **Exam-mode hard limit.** On a strong exam/quiz signal the program immediately disables all features and restores any modified policy values. The default-on "exam guard" (`EnableExamGuard`) only governs detection; turning it off stops detection but does not change this project's usage boundary — use during any exam, quiz or proctored session is prohibited.

This disclaimer is subject to change with new versions. It does not exempt users from liability and is not a substitute for legal advice.

## 许可与第三方

本项目以 MIT 许可证发布，全文见 `LICENSE`。
第三方组件：MinHook（BSD-2-Clause，见 `third_party/minhook/LICENSE.txt`）；实现思路参考 JiYuTrainer（MIT），未复制其代码。
