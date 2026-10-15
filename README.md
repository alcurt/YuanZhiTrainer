# YZTrainer

> **v0.6.42** · 一款**远志多媒体教学管理软件 V9.0 网管版学生端**的解控软件 · 仅供**教育研究**与**技术学习**，禁止任何非法用途 · 与厂商无关联，使用者自行承担全部后果。

> **部署硬要求（2026-09-29 机房实测）**：学生端以 SYSTEM 运行，而本程序默认是管理员（高完整性）。
> 两者差一级完整性，`VirtualAllocEx` 注入、`SetWindowsHookEx` 消息钩子、跨进程改窗口**三样都会被 UIPI 挡住**
> （已于 09-23、09-29 两次实测）。**必须用 `YZSysRun.exe -- YZTrainer.exe` 以 SYSTEM 身份启动主程序**：
> 同一台机器、同一个客户端，换成 SYSTEM 后消息钩子立刻落进 `Yistart.exe`（29 个 hook、管道连通）、
> 跨进程改窗口也不再报 0x5。v0.6.5 起程序在连续注入失败时会打印这条提示，并运行期自动改用消息钩子注入。

针对 **广州远志（Howyar）YZinfo 多媒体教学网络系统 V9.0 网管版学生端** 的课堂辅助工具（普通学生端同样适用，实测机房部署的是网管版），参照 JiYuTrainer 的思路重新实现（不复制其代码）。

## 它做什么

* **广播窗口化**：教师端进行全屏广播时，把全屏、置顶、无边框的广播窗口改成可自由拖动缩放的普通窗口，画面与声音继续，你可以边看老师演示边操作自己的电脑。**注入进不去也能用**：进程内 Hook 拿不到目标进程时，改由主程序跨进程改窗口样式（`ExternalWindowFix`，默认开）。
* **假全屏（默认关闭）**：广播窗口继续铺满整屏、外观不变，只把它从最上层拿下来——教师端看到的仍是"全屏广播"，而你依次按 Alt+Tab 切过去的窗口能盖在广播上面，配合键鼠解锁就能操作自己的电脑。它与窗口化互斥且**优先于窗口化**。注意它只改窗口层级，**不隐藏内容**：教师端若在抓屏，看到的还是你的真实屏幕，要遮内容得靠防监视（冻结帧）。
* **教师端远程执行审计（始终开启）**：记录学生端进程里出现的 `CreateProcessA/W`、`WinExec`、`ShellExecuteExW`、`ExitWindowsEx` 调用（教师端的"远程执行命令/关机/重启/注销"最终都落在这里），**只写日志、不拦截、不改行为**。审计文件为日志目录下的 `remote-exec.log`（超过 2MB 顺延 `remote-exec-1.log`，不删除旧文件），并把"疑似教师端下发"的条目同时写进界面日志。考试模式下连审计也停。
* **解除键鼠锁定（2026-10-09 实测结案：用户态解不开）**：把学生端安装的 `WH_KEYBOARD_LL` / `WH_MOUSE_LL` / `WH_KEYBOARD` / `WH_GETMESSAGE` 等钩子**替换成空过程**（v0.6.11 起，见“实现要点 · 输入层”），并拦下 `ClipCursor` / `BlockInput` / 逃生类热键 / SPI 改写；只处理**确实是远志自己装的**钩子与逃生组合键，不动同进程里其它组件的合法钩子/热键。**但机房实测证明它解不开封锁**：客户端进程内一次都没调过这些 API、内核 clip 矩形在整段广播期都是整屏，鼠标却仍被反复拽回左上角（设备层封锁，见“已知限制”第 14 条）。这条开关现在的实际价值是“中和钩子 + 留下证据”。
  **2026-10-15 现场状态**：那天早上在机房跑键鼠探针时，`tools\input-probe.ps1` 一传 `-TargetProc` 就崩
  （脚本里 `$P = [YzInputProbe]` 被后面的 `$p = Get-Process` 覆盖，变量名大小写不敏感）⇒ **A/B/C/D 四节一条数据都没拿到**，
  所以 10-09 的判决仍是当前最好的结论，但**尚未被 A/B/C/D 复核过**。脚本已在 v0.6.41 修好（并专门用
  `-TargetProc` 实测通过），A/B/C/D 的现场复核列为待办。**在拿到 A/B/C/D 之前，不要对外声称"已确认是设备层"。**
* **防监视（默认关闭）**：开启后教师端“监视/远程桌面”看到的是开启瞬间的静止画面（冻结帧），关闭立即恢复实时。**但它目前只覆盖"学生端进程自己用 GDI 抓屏"这一条路径**：厂商的采集层还有 DXGI（`ExdDtDup.dll`）与独立进程的 GDI helper（`ScreenRecoder.exe`），那两条路上这个开关无效——原因与后续计划见“已知限制”第 1 条。
* **拦截教师端遥控输入（默认关闭）**：开启后丢弃学生端进程内的 `SendInput` / `mouse_event` / `keybd_event` / `SetCursorPos`，教师端无法接管你的鼠标键盘。
* **服务/驱动面板**：列出与远志相关的服务与内核驱动，只做临时停止/启动，不删除任何服务或文件。

## 它不做什么（硬边界）

* 不写内核驱动、不做手动映射、不碰远志的驱动文件。
* 不修改、不替换任何远志文件；不写持久化注册表项。**v0.6.13 起连"临时改写 HKCU 策略值"也删掉了**：机房实测（G3）证明远志的封锁不走 HKCU 策略，我们那套写入既没有作用对象、又落在默认用户模板（`.DEFAULT`）上——收益为零，副作用却是真的（进程被硬杀时"退出还原"不执行）。现在 `policy.cpp` 只保留一行**只读**的落点自报（`PolicyHive: 本进程 HKCU → …`）。
* 不向其他机器发送指令、不做 UDP 攻击、不给同学机器远程发消息或执行命令。
* 不做免杀、混淆、反检测。
* **检测到考试/测验的强信号时，全部功能立即自动停用**：独立考试进程 `ExamDlg.exe`、进程内加载 `ExamDlg.exe` / `ExamDlg.dll` / `ORAL_EXAM.ocx` / `ExamEditor.exe`，或存在**可见**且标题命中考试类关键词的远志窗口。注意 `Exam.ads` / `ClassQuiz.ads` 属学生端启动即加载的常驻模块（弱信号），**只会记日志、不再触发熔断**——它们是就绪态而非"正在考试"的判据。
* 不尝试在无管理员权限的环境下工作。

## 工程结构

```
YZTrainer/
  YZTrainer.sln             VS 解决方案（6 个工程：Trainer / Hook / Probe / SimTarget / UnhookTest / SysRun）
  build.ps1                 命令行构建脚本（MSBuild；`-Both` 同时出 Win32 与 x64）
  src/common/               公共协议、日志、工具函数；**版本号唯一来源 `yz_version.h`**（三个工具共用 `yz_version.rc`）
  src/YZTrainer/            主程序：托盘 UI、热键、注入器、命名管道服务、服务面板、诊断导出
  src/YZHook/               注入到远志学生端的 Hook DLL（MinHook + 窗口/输入/采集/远程执行审计）
  src/YZProbe/              机房侦察工具（进程/模块/窗口/服务/网络快照 + 图形采集栈判定，输出 JSON + Markdown）
  src/YZSimTarget/          模拟远志学生端（本地回归测试用，故意不带版本资源）
  src/YZUnhookTest/         免注入拔钩验证工具（本进程加载远志 DLL 调其导出，不注入）
  src/YZSysRun/             以 SYSTEM 身份运行指定程序的启动器（复制当前会话 winlogon 令牌，不装服务）
  third_party/minhook/      MinHook（BSD-2-Clause，已内置源码）
  tests/run_sim_test.ps1    本地自动回归脚本（二十五阶段 62 项断言，约 4.2 分钟（v0.6.42 + 第四轮提速实测 251.9 秒；基线 383.6 秒，-34.3%），需要管理员权限），
                            阶段八是 P0"空过程替换"的回归闸门（v0.6.13，含负向 A/B 验证，
                            v0.6.15 起还覆盖 hMod=NULL 的线程内钩子），
                            阶段九看消息层钩子开关（v0.6.14），
                            阶段十看探针 --watch 的触发式采集（v0.6.15）
  dist/                     构建产物输出目录（含 `build.ps1` 写出的出厂 `YZTrainer.ini`）
  archive/                  机房实测档案（含机位名/账户/IP，**只留本地、绝不入库**）
  工作顺序.txt              每次上机的操作清单（本地文件，不入库）
  交接记录.md / 开发记录.md   会话记录：前者是别的 session 交给主线的输入、后者是主线自己的工作记录（均只留本地）
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

也可以直接用 VS 打开 `YZTrainer.sln`，选择 `Release | Win32` 生成解决方案。所有工程统一钉在 `WindowsTargetPlatformVersion 10.0.22621.0`（与机房 Win11 23H2 对齐，需装该版本或更高的 Windows SDK）；主程序清单同时声明 `requireAdministrator` 与 `PerMonitorV2` 高 DPI（换显示器会重排界面，125%/150%/175% 下都清晰）。产物在 `dist\Win32\`：

| 文件 | 用途 |
|---|---|
| `YZTrainer.exe` | 主程序（清单要求管理员权限），已内嵌 YZHook.dll，单文件即可运行 |
| `YZHook.dll` | 注入模块，运行时从 exe 资源释放；dist 下这份是回退/调试用，分发时不需要拷贝 |
| `YZProbe.exe` | 机房侦察工具，只读，建议先跑它。报告 `notes` 里会给出每个远志进程的**图形采集栈**行（`ExdDtDup.dll` / `d3d11` / `dxgi` / `gdi32` 是否已加载 + 磁盘 `KsFiles\ExdDtDup.dll` 是否存在 + 判定"DXGI 桌面复制 / GDI 抓屏 helper"），"冻结画面"要按它选钩子路径（v0.6.11 起）。v0.6.12 起：**对全部进程**轻量扫 `ExdDtDup/ExdElite/ExdPaint`（这三者可能挂在一个"看起来不像远志"的进程里），并区分"模块枚举失败"与"确实没加载"；另新增**触发式常驻模式** `YZProbe.exe --watch [--duration 秒] [--interval 秒]`——出现新远志进程、任何进程加载采集组件、或出现 `Brw:`/覆盖整屏的无边框远志窗口时立刻落一份完整报告，每分钟一条心跳 |
| `YZSysRun.exe` | **以 SYSTEM 身份运行指定程序**的轻量启动器（令牌复制，不装服务、不写注册表）：用来复测"换个身份是否就不被剥句柄权限"。用法 `YZSysRun.exe -- YZProbe.exe -o <目录> --no-pause`。它从**当前会话**的 `winlogon.exe` 复制主令牌并以 `winsta0\default` 启动子进程，因此子进程仍在交互桌面上；启动后会打印令牌来源、子进程会话与完整性级别（应为 系统(16384) 且会话号与当前一致）。**控制台的全部输出同时写成同目录 `YZSysRun-<时间戳>.txt`（可用 `--log <文件>` 指定路径，exe 目录不可写时依次退当前目录、`%TEMP%\YZTrainer\`）**，免得现场来不及另存 |
| `YZUnhookTest.exe` | 免注入拔钩验证工具：默认只体检（读文件导出表 + 调用约定自检）并把过程写入同目录 `YZUnhookTest-<时间戳>.txt`；`--apply` 才真正调用远志的拔钩导出，`--arm` 待机等你回车再执行，`--delay N` 待机 N 秒后自动执行（被锁键鼠时唯一可行的触发方式），`--no-pause` 供脚本调用。调用前后会打印对照节的**基址、大小、前 32 字节原始值**和三个 HHOOK/标志字（v0.6.2 起按**节名+节内偏移**定位；此前写死的 RVA 差了两个数量级，所以在现场报告里从来没出现过对照行） |
| `YZSimTarget.exe` | 模拟学生端，仅本地测试用。`--exam` 造考试窗口、`--exec` 4 秒后拉起子进程（验证审计）、**`--load <dll>` 4 秒后加载指定 DLL**（验证"钩子模块晚于注入出现"时主动拔钩的重试，见阶段五）、**`--hook` / `--hook-thread` 像厂商那样自己装键盘钩子并把 `NULL` 当生死判据**（v0.6.13，P0"空过程替换"的回归替身，见阶段八）、**`--hook-message` 额外再装一个 `WH_GETMESSAGE`**（v0.6.14，验证消息层开关，见阶段九）；`--hook*` 都配合把 exe 放进路径含 `GZYZ` 的目录使用，且都是启动 8 秒后才装 |

上表里会离开本机的四个工具（`YZTrainer.exe` / `YZProbe.exe` / `YZSysRun.exe` / `YZUnhookTest.exe`）自 v0.6.10 起带版本资源，`FILEVERSION`/`PRODUCTVERSION` 与字符串字段统统由 `src\common\yz_version.h` 单点提供——现场比对"U 盘上这份是不是最新"可以直接看文件属性，不必再靠字节数猜。`yz_protocol.h` 里派生的字符串与那份数值宏由一条 `static_assert` 互相咬合，只改一处会直接编译失败。`YZHook.dll` 故意不加版本资源：它是注入载荷，写 `CompanyName`/`ProductName` 等于给教师端软件留下可识别特征，它的身份已经由 `YZHook_x86_<hash>.dll` 这个文件名唯一确定。

## 建议的使用流程

1. **本地先跑通**：管理员 PowerShell 执行 `pwsh -File tests\run_sim_test.ps1`，确认二十五个阶段共 **62 项断言**全通过——阶段一（注入路径）6 项：全屏置顶 / 窗口化 / 带 WS_CAPTION / 冻结画面 / 不受保护的目标仍走远程线程注入（v0.6.14）/ 注入预检没有误判（v0.6.14）；阶段二（免注入路径）3 项；阶段三（假全屏）2 项；阶段四（远程执行审计）2 项；阶段五（主动拔钩的晚加载重试，v0.6.3）1 项：`YZSimTarget --load` 在注入之后才加载一个名叫 `ExdHooks.dll` 的替身 DLL，日志里必须出现"NativeUnhook: 目标模块 ExdHooks.dll 已加载，开始尝试"；阶段六（反横跳闸门，v0.6.6）1 项：`YZSimTarget --fight` 每 200ms 抢回无边框全屏（复现机房"全屏↔窗口横跳"），外部纠正必须在几次短间隔抢回后**认输停手**（日志出现"已对该窗口停手"）；阶段七（采集栈监控，v0.6.12）2 项：某进程加载 `ExdDtDup.dll` 时日志必须出现"采集栈监控: PID=… 出现采集组件 ExdDtDup.dll"，且自动导出的诊断包 `snapshot.txt` 里含"== 采集栈快照 =="与该命中；阶段八（P0 空过程替换，v0.6.13）2 项：把模拟目标复制到**路径含 `GZYZ`** 的目录里（命中 `IsYuanzhiInstallPath` 的子串判据，不需要在生产代码里开后门）并用 `--hook` 让它像厂商那样"自己装钩子、拿 `NULL` 就自杀"，此时日志必须出现 `已用空过程替换远志键盘/鼠标钩子… hhook=0x非零`，且该进程**仍然存活**；阶段九（消息层钩子开关，v0.6.14）2 项：`BlockMessageHooks=1` 时键盘与消息层两类钩子都被换成空过程，`=0` 时只放行消息层、键盘钩子照旧被换掉；阶段十（探针 `--watch`，v0.6.15）2 项：常驻探针 + 某进程加载改名成 `ExdDtDup.dll` 的替身 → 必须落一份 `-watch-capture` 报告且内容命中，并按 `--duration` 正常退出（watch 日志有"watch 结束：共落盘 N 份报告"）；阶段八第二段（v0.6.15）2 项：`--hook-thread`（`hMod=NULL`）的线程内钩子同样被换成空过程且客户端存活；阶段十一（考试熔断，v0.6.16）2 项：`YZSimTarget --exam` 造出标题含"考试"的可见窗口时，若 `EnableExamGuard=1` 必须熔断且功能真的停（状态仍 `fullscreen=1`），对照组 `=0` 时钩子侧记录"考试模式检测已按配置关闭"且功能恢复；阶段十二（`YZSysRun` 令牌自检，v0.6.17）2 项：它自报的令牌必须是 `NT AUTHORITY\SYSTEM`/系统完整性，且子进程留在当前交互会话；阶段十三（免注入假全屏，v0.6.18）2 项：`AutoInject=0` + `ExternalWindowFix=1` + `Flags=36` 时，跨进程纠正也要做出假全屏（状态 `fullscreen=1` 且 `topmost=0`、日志有"外部窗口纠正(假全屏)"），且不给窗口加标题栏；阶段十四（身份不足时的判读，v0.6.19）2 项：目标是 SYSTEM、主程序是管理员时，跨进程窗口写必须被 UIPI 拒绝且**不静默**（记 WARN、窗口仍全屏）；换成 `YZSysRun` 以 SYSTEM 启动主程序后同一操作成功（状态由 `fullscreen=1` 变 `fullscreen=0`）——这就是 09-23 误判 / 09-29 判决的本地对照；阶段十五（冻结开启时别把自己拍进去，v0.6.20）2 项：防监视开启时主界面自动收进托盘（进程无可见主窗口），且主窗口亲和性为 `WDA_EXCLUDEFROMCAPTURE`（进程内读回 `0x11`）并确实冻住画面；阶段十六（DXGI 取证，v0.6.21）2 项：`TraceDesktopDuplication=1` 时 `[DXGI 取证] 建立桌面复制成功` 与 `[DXGI 取证] 周期 取帧统计` 都要出现，且模拟端确实取到帧（钩子只计数、不改行为，默认关闭）；阶段十七（探针跨架构，v0.6.22）2 项：**x64** 探针在 32 位目标上必须枚举出模块并判出"采集层=DXGI"，全进程扫描也要命中该目标（漏 `TH32CS_SNAPMODULE32` 时不会报错、报告照样生成，只会模块列表为空——现场会误判"没有采集组件"）；阶段十八（探针保护级别，v0.6.23）2 项：仅因"采集组件扫描"晋级的进程必须出现在"远志相关进程"表里，且其"进程保护"列是"无/无保护"、**不能**是 `PPL/WinTcbLight`（修掉的 bug 正是把"查不到"当成了 WinTcb Light 这一档）；阶段十九（x64 探针 `--watch` 跨架构，v0.6.24）2 项：现场采"教师监视时谁在抓屏"的主力通道是 **x64** 探针的 `--watch`，它走的是另一套代码（常驻轮询+触发判定+落盘），必须在 32 位目标上也能触发并给出 `采集层=DXGI` 判词；阶段二十（`InjectMethod=1` 消息钩子注入，v0.6.25）3 项：这条是 README 给现场的兜底注入办法，此前只测过默认的 0 与运行期自动回退——现在断言"日志出现 `已通过消息钩子注入 HHOOK=…` + 窗口化照常生效 + 不再尝试远程线程注入"；阶段二十一（界面/热键那条开关链，v0.6.26）3 项：`CmdSetFlag` 对四个功能位**只发 `YZ_CMD_SET_*` opcode、不发整份配置**，所以用户勾复选框/按热键走的是钩子侧 `EngineSetFlag` 那条链——用 `WM_COMMAND`+`BM_SETCHECK` 驱动复选框、直接投递 `WM_HOTKEY(F12)` 驱动热键处理器，断言防监视能开能关（哈希停住→又变化）与主界面能隐藏能恢复；阶段二十二（托盘菜单路径，v0.6.27）2 项：托盘菜单能打开防监视（`Flags` 出现防监视位 + 哈希停住），且冻结收起主界面后托盘"显示主界面"能恢复界面**而不解开冻结**——`v0.6.20` 起冻结会把主界面收进托盘，这条入口是冻结期间唯一的图形开关；阶段二十三（ini 边界项，v0.6.28）3 项：`WindowPercent=5` 必须被钳成 20%、`=200` 钳成 100%（且仍是带标题栏的窗口），三个 `AutoExportOn*` 全关时阶段期间不得新增 `diag-*-auto` 目录；阶段二十四（诊断包自带各来源日志，v0.6.29）2 项：自动导出的包里必须带 SYSTEM 侧日志（`yzt-sys.log`）且 `snapshot.txt` 写明来源，日志目录里现存的每个 `yzt*.log`/`remote-exec*.log` 都要有副本；阶段十一（考试熔断，v0.6.16）2 项：`YZSimTarget --exam` 造出标题含"考试"的可见窗口（进程在远志路径下）时，必须熔断且功能真的停（窗口不被窗口化、状态仍是 fullscreen=1），对照组 `EnableExamGuard=0` 时钩子侧记录"考试模式检测已按配置关闭"且功能恢复。
2. **机房侦察（先跑探针）**：把 `YZProbe.exe` **两份架构都带上**，右键以管理员运行，生成 `YZProbe-report-*.json/.md`。
   **主力用 32 位那一份**：学生端 `Yistart.exe` 与抓屏子进程（`SysWOW64\rundll32` + `KsFiles\ExdDtDup.dll`）
   都是 32 位，只有同架构的探针才枚举得到它们的模块、判得出"采集层=DXGI"；**x64 那份只作交叉验证**，
   它对 32 位目标会报"模块枚举失败"，那是预期而不是故障（阶段十七/十九就是覆盖这条跨架构行为的）。
   2026-10-15 现场教训：清单里当时写成"x64 版本信息最全"，容易让人只带 x64，从而看不到 32 位目标的模块。
   重点看：
   * “远志相关进程”里哪个进程加载了 `Rmdesk.ads` / `PlayerGUI.dll` / `ExdHooks.dll`；
   * 远志模块导出表里 `KillHook` / `UnSetExdHooks` / `UnSetExdHooks2` 是否存在（与本地版本比对，决定主动拔钩可用性）；
   * “疑似全屏/置顶窗口”里广播窗口的类名、样式与所属 PID（确认是否由 `ExdPaintHelper.exe` 承载）；
   * “远志相关服务/驱动”的 ImagePath，确认文件过滤/网卡过滤驱动的真实名字；
   * 进程模块里是否出现 `ExdDtDup.dll`（决定防监视能不能用，以及**该钩哪条路**）；v0.6.12 起这一项由两处**自动**采：探针 `--watch` 常驻，以及主程序常驻监控（见"已知限制"第 1 条）；`self` 段的完整性级别/提权/SeDebug。
   * **“注入能力实测”表的 `句柄实得权限` 一列，以及每个进程下面的“逐权限位实测”子表**：判定为 **被削**（`OpenProcess` 成功、但句柄实得权限里没有请求的那一位）是内核驱动用 `ObRegisterCallbacks` 一类回调剥权限的铁证；`打不开（无权限）` 才是真的权限不够。这两处过去一直显示“未取到”（`NtQueryObject` 缓冲区长度 bug，v0.5.1 修复），所以历史报告里这一列为空**不等于**没有被削。
   * **尽量在教师正在广播时再采一份**：只有广播进行中才会出现广播窗口，"疑似全屏/置顶窗口"表里的那行（类名/样式/所属 PID）是判断窗口化判据够不够用的唯一硬证据。2026-09-18 的两份报告都是无人广播时采集的，这一点至今仍是空白。
3. **报告留在 `YZTrainer.exe` 同目录**：诊断包会自动把它一起打包，省得漏带。
4. **（可选，但推荐）再用 SYSTEM 身份复测一次探针**：在被剥句柄权限的机器上，
   用管理员 PowerShell 跑
   `YZSysRun.exe -- YZProbe.exe -o <输出目录> --no-pause`
   ——启动器会打印"复制令牌: 用户=NT AUTHORITY\SYSTEM 会话=1 完整性=系统(16384)"与子进程的会话/完整性，
   确认子进程**既真的是 SYSTEM、又仍在交互会话**（若会话号与当前不一致，探针的窗口表会是空的，那种数据不要用）。
   然后把两份报告并排看"注入能力实测"的逐权限位子表：同一台机器上，如果 SYSTEM 身份下"被削"的位消失，
   说明保护是按调用者身份判定的，注入路线还有救；如果照样被削，说明驱动不认身份，就该转向免注入路线。
   注意 `服务/驱动` 一栏只列远志相关服务（所以 0 条很正常），完整非微软驱动清单在 json 的 `thirdPartyDrivers` 里。
5. **机房实测（再跑主程序）**：把 `YZTrainer.exe` 单文件拷过去，**先确认没有别的 YZTrainer 在跑**（全局互斥体，第二份会静默退出），管理员运行，日志里按顺序确认三件事：
   * `目标进程 pid=… (…) 命中: …`（应优先命中“广播窗口宿主”）；
   * `已注入目标进程 pid=…` 与 Hook 侧的 `Hook 安装完成，共 28 个`；
   * `NativeUnhook: 已调用 …`（远志模块已加载时）——这是“能否解掉已经锁死的机器”的直接证据。
   随后让教师开始广播，看广播窗口是否变成可操作窗口、键鼠是否可用；`Ctrl+Alt+F8/F9/F10/F11/F12` 是对应开关，注意**假全屏与防监视默认关，只在明确要演示时按**。
   如果日志里只有 `注入失败: VirtualAllocEx 失败 …（拒绝访问 …）`，请把 `InjectMethod` 改成 `1` 再试消息钩子；两条路都不通时，窗口化仍由外部纠正负责，日志里会出现 `外部窗口纠正: 0x… -> x,y wxh`，状态栏“窗口化次数”的第二项就是它的计数。
6. **导出诊断包**：它是**即时快照**，在“你希望被记录的那个状态”下导出。建议导两份——一份正常基线、一份出问题的那一刻；只能带一份回来时优先带异常那份。输出在 exe 同目录 `diag-<时间戳>\`，含日志（**各来源都带**，见下）、`snapshot.txt`（配置/运行状态/进程/目标窗口/服务）与同目录的 `YZProbe-report-*`。退出程序后确认 hook 已卸载、被改写的策略值已还原。
   **v0.6.8 起不必依赖"手点"**：程序会**自动静默导出**——① 首次识别到广播窗口时（`diag-<时间戳>-auto`，快照首行写着"导出方式: 首次识别到广播窗口（宿主侧扫描 / 客户端 Hook 上报）"）② 退出时（"导出方式: 退出"）③ **v0.6.12 起加一份：首次发现采集组件（`ExdDtDup`/`ScreenRecoder` 等）时**（"导出方式: 首次发现采集组件（ExdDtDup/ScreenRecoder 等）"）。动机：2026-10-08 那次广播没有预告、一节课只有一次，等反应过来工具还没启动，广播态的证据整段丢失；而"教师监视时谁在抓屏"同样只在几秒里出现，它决定冻结画面走 DXGI 还是 GDI。现在只要**开课前把程序挂上、整节课别关**，这三件事件各自动留下一份包。开关分别是 `AutoExportOnBroadcast` / `AutoExportOnExit` / `AutoExportOnCapture`。
   **v0.6.12 起每份诊断包的 `snapshot.txt` 都带 `== 采集栈快照 ==` 一块**：本次运行期间观察到的采集组件命中（PID/进程/模块/路径 + "该走哪条冻结路径"的判词），以及当前远志相关进程的逐进程图形采集栈判定与关键模块。主程序每 30 秒扫一轮（新出现的进程在 30 秒观察窗内每轮都重扫，另有 5 分钟一次的全进程兜底扫）。

## 界面与热键

| 热键 | 功能 |
|---|---|
| `Ctrl+Alt+F8` | 切换“假全屏（保持全屏外观，仅取消置顶）” |
| `Ctrl+Alt+F9` | 切换“广播窗口化” |
| `Ctrl+Alt+F10` | 切换“解除键鼠锁定” |
| `Ctrl+Alt+F11` | 切换“防监视（冻结画面）” |
| `Ctrl+Alt+F12` | 显示/隐藏主界面 |

热键是**全局**注册的：若某个组合已被别的程序占用（实测本机 `Ctrl+Alt+F9` 就被占用，
`RegisterHotKey` 返回 `err=1409`），启动日志里会出现 `注册全局热键失败: …`（v0.6.26 起不再静默），
此时用界面复选框或托盘菜单仍可切换；`Ctrl+Alt+F12` 的"隐藏"也是 v0.6.26 才真正生效（此前只显示、不隐藏）。

其中**窗口化、解除键鼠锁定默认开；假全屏、防监视、拦截遥控默认关**（即默认 `Flags=5`）。假全屏与窗口化同时打开时以**假全屏**为准。托盘右键菜单有对应开关与“立即注入”的等价入口，热键被其它程序占用时用菜单。

托盘交互与主流 Windows 程序一致：**左键单击显示并前置主界面，右键单击打开菜单**（双击同样按左键单击处理）；第一次把窗口关掉或最小化时，会弹一条气泡提示“窗口已隐藏到托盘”，避免误以为程序退出了。

配置文件 `YZTrainer.ini`（与 exe 同目录，目录不可写时仅内存生效）：

```ini
[General]
Flags=5            ; 1=窗口化 2=窗口置顶 4=键鼠解锁 8=防监视 16=拦遥控 32=假全屏
WindowPercent=60   ; 窗口化后的宽度占屏幕百分比（20-100）
LogLevel=2         ; 0=错误 1=警告 2=信息 3=调试
AutoInject=1       ; 是否自动注入 / 客户端被服务重启后自动补注入
InjectMethod=0     ; 注入方式：0=CreateRemoteThread+LoadLibrary（默认）；1=SetWindowsHookEx 消息钩子
                   ; v0.6.13 起注入前先读一次句柄实得权限：缺 PROCESS_VM_OPERATION 时（内核剥夺，
                   ; 机房实测实得恒为 0x1402）直接跳过注定失败的远程线程注入，不再白试 3 次
TargetDir=         ; 远志安装目录，留空=自动判定
HookDllPath=       ; 留空=用内嵌的 YZHook.dll；填路径可强制改用外部 DLL（调试用）
EnableExamGuard=1  ; 考试模式守护：1=仅强信号熔断（默认）；0=完全跳过考试检测
ExternalWindowFix=1; 免注入的外部窗口纠正：1=开（默认）0=关，只在窗口化开关打开时生效
AutoExportOnBroadcast=1; 首次识别到广播窗口时静默导出一份诊断包（默认开，每进程一次）
AutoExportOnExit=1 ; 退出时静默导出一份诊断包（默认开）
AutoExportOnCapture=1; 首次发现采集组件（ExdDtDup/ScreenRecoder 等）时静默导出一份（默认开，每进程一次）
BlockMessageHooks=1; 是否也把远志的 WH_GETMESSAGE/WH_CALLWNDPROC 换成空过程（默认开）
TraceDesktopDuplication=0; 是否装 DXGI 桌面复制"取证"钩子（默认关；开了**只计数、不改变任何行为**，
                        用来判"教师监视/广播到底有没有走桌面复制这条链"，日志里出 [DXGI 取证] 前缀）
                   ; 现场出现“广播画面不动/窗口消息异常”时改成 0 重启即可做二分，不必重编（v0.6.13）
ProcessNames=Yistart.exe;TEACHCMD.exe;PlayerGUI.exe;ExdPaintHelper.exe
```

日志：`%TEMP%\YZTrainer\yzt.log`（超过 2MB 自动换用 `yzt-1.log`，不删除旧文件）；远程执行审计单独写在同目录 `remote-exec.log`。**注意有第二份**：用 `YZSysRun` 以 SYSTEM 启动的主程序，它的日志写在 **SYSTEM 的 `%TEMP%`**，即 `C:\Windows\Temp\YZTrainer\yzt.log`（v0.6.19 实测确认）——现场手工捞日志时要两份都拿，或直接拿主程序自己导出的诊断包（它只包含自己那一份，不会混）。
**v0.6.3 起 Hook DLL 的日志会镜像回主程序**：此前 hook DLL 写的是**目标进程**的日志目录（目标以 SYSTEM 运行时是 `C:\Windows\Temp\YZTrainer\`），那些行永远进不了宿主导出的诊断包——现场只剩显式转发的那几行，`HookAttach` 失败、主动拔钩跳过、策略备份这类关键证据会全部丢失。现在引擎注册了 `yz::LogSetSink`，目标进程里每一条 `YZLOG*` 都随管道回到宿主日志（形如 `[Hook pid=…] [时间][TID][YZHook][INF] …`）。注意：**注入没成功时没有这层镜像**，那种情况下仍然是"日志断链"的状态。

## 实现要点

* **注入**：主程序启用 `SeDebugPrivilege`，`CreateRemoteThread` + `LoadLibraryW` 把内嵌在 exe 资源里的 `YZHook.dll` 注入目标进程（释放到 `%ProgramData%\YZTrainer\cache\`、按内容哈希命名、先校验 PE 架构）；每 2 秒巡检，客户端被服务重启后自动补注入。
* **目标选择（三路打分 + 一路兜底）**：① 正在全屏广播的窗口宿主 PID，**+5**，判据与 Hook 侧完全一致（无属主 / 无标题栏 / 非子窗口 / 非桌面壳类 / 覆盖整块显示器 / POPUP 或置顶）；② 进程名在 `ProcessNames` 名单，**+3**；③ 安装路径含 `YZinfo Multimedia teaching software` 或 `GYZY`，**+2**。三者都没命中时，才逐个查进程模块是否加载了 `Rmdesk.ads` / `PlayerGUI.dll` / `ExdHooks.dll`（代价高，仅兜底）。选中后在日志里写明命中原因。
* **主动拔钩（只在本进程真正持有钩子时才有意义）**：用户态没有受支持的 API 能摘掉**别人**装的钩子，但远志自己导出了卸载入口——解锁开关生效时调用 `KeyboardHook.dll!KillHook()`、`ExdHooks.dll!UnSetExdHooks(0)`、`ExdHooks.dll!UnSetExdHooks2(GetCurrentThreadId())`。注意 `UnhookWindowsHookEx` 只能由**安装钩子的那个进程**调用，所以这三个入口只有在"远志确实把钩子装进了当前进程"时才会真起作用。调用放在独立工作线程并限时等待（`KillHook` 结尾是 `WaitForSingleObject(..., INFINITE)`，直接在引擎线程调用有挂死风险），调用点用 SEH 兜底。
* **调用约定自检（fail-closed）**：调用远志自带导出前，用内置的 HDE32 走真实指令流确认函数以 `ret` / `ret 0` 收尾、单参函数确实读取 `[esp+4]`；出现 `ret imm16 != 0`、远返回、thunk(`jmp`) 或解析失败一律拒绝调用并记 ERROR。裸字节扫描会被指令里的 ModRM 字节骗到（例如 `3B C3` 里的 `0xC3`），所以必须走解码。
* **备用注入方式（消息钩子）**：`InjectMethod=1` 时，主程序用 `SetWindowsHookEx(WH_GETMESSAGE)` 把已释放的 `YZHook.dll` 挂到会话的 GUI 线程上，由系统加载器把 DLL 映射进目标进程——我们自己不写目标内存，因此能绕开"内核组件剥夺句柄 VM 权限"这一类保护（`OpenProcess` 成功、`VirtualAllocEx` 返回 0x5 的场景）。代价是 DLL 会被映射进本会话所有 GUI 进程（宿主识别不通过的进程里只是空转），因此默认仍用远程线程方式，只在被剥夺句柄权限的机器上切到 1。
* **注入能力诊断（YZProbe）**：探针用 `GetProcessInformation(ProcessProtectionLevelInfo)` 读进程保护级别（`PROTECTION_LEVEL` 枚举，`0xFFFFFFFE`=无保护）、用 `NtQueryObject` 读句柄**实得权限**，再逐项实测模块枚举 / `ReadProcessMemory` / `VirtualAllocEx` / `WriteProcessMemory`（只写自己刚分配的一页并立刻释放）。`--access <pid>` 可对单个进程单独诊断。它同时列出非微软签名的内核驱动（保护件/杀软/还原卡）与服务的 `SERVICE_CONFIG_LAUNCH_PROTECTED` 标志。
* **免注入拔钩（YZUnhookTest）**：工具在**本进程**里 `LoadLibrary` 远志那两个 DLL，再调用 `KillHook()` / `UnSetExdHooks(0)` / `UnSetExdHooks2(GetCurrentThreadId())`，完全不需要注入目标进程。**2026-09-23 静态纠正**：原先假设"两个 DLL 都靠跨进程共享节传递 HHOOK"，节表只对了一半——`ExdHooks.dll` 的 `.ExdHook` 是真共享节（flags `0xD0000040`，含 `MEM_SHARED`，RVA 0x7000，56 字节），而 `KeyboardHook.dll` 的 `HookData` **不是**（flags `0xC0000040`，没有共享位，导入表里也没有任何 file-mapping/共享内存 API），它在每个进程里都是私有副本。所以"在本进程调 `KillHook` 去拔远志的钩子"**结构上就不成立**，机器上表现为"调用成功但毫无效果"。工具默认只体检（读文件导出表 + 调用约定自检），`--apply` 才真调用，并在调用前后打印对照节的基址/大小/前 32 字节原始值与三个 HHOOK/标志字——**这些值全为 0 就说明那一层当时根本没装钩子**，这是判断"锁在哪一层"的第一手证据。
* **窗口层（两种模式）**：hook `SetWindowPos` / `MoveWindow` / `ShowWindow` / `SetWindowLongA/W`，命中“本进程 + 无标题栏 + 覆盖整块显示器 + 置顶或 POPUP”的窗口后按模式处理——**窗口化**改写 `WS_OVERLAPPEDWINDOW`（默认屏宽 60% 居中、不抢焦点），**假全屏**保留 `WS_POPUP` 全屏外观、只把 `WS_EX_TOPMOST` 去掉并强制 `HWND_NOTOPMOST`；两者都在客户端每 3 秒抢回全屏时按 500ms 节流重新纠正，假全屏另有"已达稳态就不再改"的判据，避免每秒白改一次。
* **外部窗口纠正（免注入兜底）**：窗口样式是会话级对象，改别人的窗口不需要目标进程配合。主程序每秒按**同一套结构判据**枚举顶层窗口，命中"属于远志进程 + 无属主 + 无标题栏 + 非子窗口 + 非桌面壳类 + 覆盖整块显示器 + POPUP 或置顶"后用跨进程 `SetWindowLongPtrW` + `SetWindowPos(..., SWP_ASYNCWINDOWPOS)` 改成普通窗口（`ExternalWindowFix`），节流 1 秒；`Flags` 里给的是假全屏时同样支持跨进程做假全屏。这是"客户端被剥夺句柄权限、`VirtualAllocEx` 返回 0x5"时唯一还能生效的窗口手段；进程内 Hook 已生效的宿主进程会被跳过（避免两边抢同一个窗口），其余远志进程仍由它兜底。写入被拒（UIPI / win32k 权限检查）会记 WARN 并在状态栏显示"有写入失败"，不静默失败。
* **远程执行审计层**：hook `kernel32!CreateProcessA/W`、`WinExec`、`user32!ExitWindowsEx`，以及（仅当目标进程已加载 `shell32` 时）`shell32!ShellExecuteExW`。一律**先调用原函数拿到真实结果，再写审计**，不做任何拦截。审计行同时落 `yzt.log` 侧的 INFO 与 `remote-exec.log`，按 exe 是否在远志安装目录里标注 `origin=self|remote`，界面日志只播报 `remote` 的条目。不为了审计去加载 shell32。
* **输入层**：把学生端安装的键盘/鼠标钩子**替换成我们 DLL 里的空过程**（v0.6.11 起；命中判据不变——**线程内钩子（`hMod == NULL`）按回调地址反查所属模块**，只有落在远志目录里才处理，归属不明的放行并记账，避免误伤输入法/公共控件）。**为什么不再返回失败（P0）**：2026-10-09 实测，把 `SetWindowsHookEx` 拦成 `NULL` 会让客户端在 8 毫秒内自杀、守卫每 4 秒把它拉起来，形成 10 秒级重启循环（电子点名窗口因此立不住）；改成“返回合法 HHOOK + 空过程”后客户端不再重启，而它的过滤逻辑照样不执行，日志文案为 `已用空过程替换远志键盘/鼠标钩子(W/A)`。`RegisterHotKey` 只拦 Win / Alt+Tab / Alt+Esc / Alt+F4 / Alt+Space / Ctrl+Esc / Ctrl+Shift+Esc 这些“逃生键”；`SystemParametersInfoW` **和 `SystemParametersInfoA`**（屏保/快速任务切换；v0.6.3 补上 A 版——`KeyboardHook.dll` 导入的正是 A 版，只挂 W 会漏）、`ClipCursor`、`BlockInput` 按开关放行或替换；`InputEnforceTick` 每秒强制 `ClipCursor(NULL)` / `BlockInput(FALSE)` 一次。v0.6.13 两点变化：① 消息层那两类钩子（`WH_GETMESSAGE`/`WH_CALLWNDPROC`）由 ini 项 `BlockMessageHooks` 控制，现场可二分；② **不再有任何 HKCU 策略读写**（原来那套在后台恢复"任务管理器/锁屏/Win 键/GameDVR"的写入已整段删除，见"已知限制"第 15 条）。拦截遥控输入时 `SendInput` 返回“全部投递成功”而不是 0，减少被学生端察觉后重试的概率。
* **采集层**：跟踪进程内取得的屏幕 DC（`GetDC(NULL)`/`GetWindowDC`/`CreateDCW("DISPLAY")`），冻结时把 `BitBlt`/`StretchBlt`/`PrintWindow` 的源改为开启瞬间抓取的 DIB，从而只影响学生端自己抓屏，不影响你本地显示。**注意这条只覆盖 GDI 抓屏路径**；厂商的采集层有 DXGI（`ExdDtDup.dll`）与 GDI（`ScreenRecoder.exe` 独立进程）两条实现，所以防监视默认关闭；v0.6.12 起主程序会**常驻监控**这两条路径何时出现，并把结果写进每份诊断包的 `== 采集栈快照 ==`。
* **通信**：命名管道 `\\.\pipe\YZTrainer`，帧格式 `[magic 'YZT1'][opcode][len][payload]`；Hook DLL 主动连接并上报状态与日志。
* **考试模式判定（强/弱信号分级）**：弱信号（`Exam.ads` / `ClassQuiz.ads` 已加载）只按节流记 INFO；强信号（独立 `ExamDlg.exe` 进程、进程内加载考试对话框组件、或**可见**且标题命中 `考试|测验|答题|试卷|快问快答|Exam|Quiz` 的远志窗口）任一命中才熔断并恢复策略、清零功能位。判据走严格目录前缀比较（避免 `...V9.0 StudentX` 这类前缀误判），窗口用 `IsWindowVisible` 过滤隐藏窗口；熔断时输出诊断（命中原因 / 考试进程路径 / 强信号模块 / 进程模块清单）到日志与 `%TEMP%\YZTrainer\exam-debug.txt`，同内容 30 秒节流 + FNV-1a 去重。开关 `EnableExamGuard`（默认 1）以控制位 `YZ_CFG_EXAM_GUARD` 随配置下发，用 `YZ_FLAG_FUNCTION_MASK` 剔除，保证功能位上报不被污染。**钩子侧该位初值是"开"（fail-closed）**：注入之后到主程序下发配置之间约 1 秒内即使 ini 写着 0 也可能先熔断一次，随后 `EngineApplyConfig` 会清掉熔断状态并打 `考试模式检测已按配置关闭(EnableExamGuard=0)`——现场判据是**后半行有没有出现**，早期那一条熔断记录属设计行为。
* **还原**：收到卸载指令或客户端进程退出时，全量 `MH_DisableHook` + `MH_RemoveHook` + `MH_Uninitialize`，释放冻结位图；不调用 `FreeLibrary`（避免崩溃），DLL 留在进程内但完全停用。（v0.6.13 起不再有“恢复策略值”这一步——已经没有策略写入了。）

## 已知限制

1. **防监视只覆盖 GDI 路径，厂商的采集层有两条实现（2026-10-11 本机反汇编新增）**：本机 dumpbin 实测（学生端与教师端副本一致）`KsFiles\ExdDtDup.dll` 依赖 `d3d11.dll`、导入 `D3D11CreateDevice`，导出 `InitDD`/`DestroyDD`/`DtEnableSnatch`/`DtDisableSnatch`/`DtDoCopy`/`DtGetBuffer`/`DtReleaseBuffer`/`RunCapture`（DD = Desktop Duplication），且**不导入任何 GDI 抓屏 API**（`BitBlt`/`StretchBlt`/`PrintWindow`/`GetDC` 全无）。反汇编进一步定位了它的调用点：`IDXGIOutput1::DuplicateOutput` 在 vtable 槽 22（偏移 `0x58`）、`IDXGIOutputDuplication::AcquireNextFrame` 在槽 8（偏移 `0x20`）、**光标由它自己用 `GetCursorInfo`/`GetIconInfo` 合成**、帧经共享节（`CreateFileMappingW`/`MapViewOfFile`）交给消费端、每块显示器一个采集线程一个 Duplication 对象；`DtEnableSnatch`/`DtDisableSnatch` 在这个版本里是空桩。另一条实现是 **GDI**：`ScreenRecoder.exe`（MFC42 时代，导入 `BitBlt`/`StretchBlt`/`GetDIBits`/`GetWindowDC`）由 `Rmdesk.ads`（远程桌面/监视）、`CmdProc.ads`、`ProxyM.exe` 引用——它是**独立进程**，不在我们的注入目标里。所以"教师监视时到底走哪条"必须看现场进程与模块。
   因此 GDI 冻结在 DXGI 路径上必然失效、在 `ScreenRecoder.exe` 那条路上也失效（钩子不在那个进程里）——**防监视保持默认关闭**。2026-10-10 的探针在广播接收期间**没有**观察到 `ExdDtDup.dll` 被加载（当时只有 `Nmdeputy.exe` + `Yistart.exe` 在跑，属"接收广播"而非"教师抓本机屏"），所以"教师监视/抓屏时由哪个进程加载它"仍待机房采一次。
   **槽位偏移已在本机功能验证过（2026-10-11）**：写了个一次性程序，真的建一次桌面复制然后**按槽位取函数指针调用**——`IDXGIOutput1::DuplicateOutput`(0x58)、`IDXGIOutputDuplication::AcquireNextFrame`(0x20)、`ReleaseFrame`(0x38) 全部返回 `S_OK`，帧信息结构被正确填充；这些指针都落在 `dxgi.dll` 里，且**同一进程内多个 Duplication 实例共享同一份 vtable**（按 vtable 打一次即可覆盖后续新建对象）。过程与输出见 `开发记录.md` §19。
   **v0.6.12 起这条取证链已常驻化**（不再靠人守）：探针 `--watch` 出现新远志进程/采集组件加载/疑似广播窗口就立刻落报告；主程序每 30 秒扫一轮，首次发现采集组件就记日志并静默导出一份诊断包（快照含 `== 采集栈快照 ==`）。**冻结画面本身仍未实现**：要覆盖 DXGI 路径需在 `hooks_capture.cpp` 追加 `IDXGIOutput1::DuplicateOutput`(0x58) + `IDXGIOutputDuplication::AcquireNextFrame`(0x20) 的 vtable 钩子（设计见 `开发记录.md` §17.1：冻结期间让 `AcquireNextFrame` 返回 `DXGI_ERROR_WAIT_TIMEOUT`，厂商会把它当"无新帧"而跳过光标合成与发布，画面与光标一起定格；若该路径会触发厂商重建，则退回"覆盖帧像素并钉住光标"）。 **DXGI 冻结的实现要点已在本机预演钉死（2026-10-11，见 `开发记录.md` §27）**：① 钩法用“换**返回对象**的 vtable”（拷贝原表、只替换槽 8/14），不动 `dxgi.dll` 里的共享表；② 桩**必须按调用方给的 `timeout` 等一等再返回** `WAIT_TIMEOUT`（上限 100ms，`timeout==0` 用 8ms）——否则厂商的采集线程会变成死循环（实测立即返回时被调用 **354 万次/秒**，一个核跑满；“真取一帧再释放”的排空策略也一样忙等，只是慢 7.6 倍）；诚实实现下是 32 次/秒、冻结照样生效；③ **不需要排空**，解冻直接恢复转发即可，长冻结 30 秒也不会 `ACCESS_LOST`；④ 预演用的是我们自己写的消费端，所以“厂商在**从未收到过任何一帧**时会显示什么”本机答不了，那一条仍要靠机房；若是黑屏/无信号，缓解办法是“放行且只放行一帧”。
2. ~~若键鼠封锁来自内核驱动或 `WinIo.dll` 端口 I/O，需要改用“指令层拦截”~~ **已作废（2026-10-09/10）**：键鼠封锁已实测结案为设备层，且按硬边界不再投入；“在 `CmdProc.ads`/`NetControl.ads` 分发点丢命令”等于篡改厂商协议，不做。保留此条只为记录曾经的判断。
3. 注入需要管理员权限与足够完整性级别；若学生端以 SYSTEM 运行而你在受限账户下，会失败（机房默认管理员登录则不受影响）。
4. 载荷已内嵌（RCDATA）：运行时释放到 `%ProgramData%\YZTrainer\cache\YZHook_<arch>_<hash>.dll`，复用前整文件比对；目录不可写时退 `%TEMP%\YZTrainer-cache\`，再失败才回退 exe 同目录的 `YZHook.dll`。释放出来的文件保留不删，便于排障。
5. 防监视从设计上只影响学生端自己抓屏的路径，不会改动系统显示驱动。
6. **同一时间只能运行一份**：主程序用全局互斥体 `Global\YZTrainer_SingleInstance` 防重入，第二份会弹提示后退出。若另一份是**非提权**的旧副本，它还会因完整性级别不够而 `OpenProcess` 失败（错误 0x5），表现就是“什么都没发生”——现场先确认没有其它 YZTrainer 在跑（含 `YZTrainer-noelev.exe` 这类改名副本）。
7. **主动拔钩的真实边界（2026-09-23 逐条反汇编核对）**：
   * `KeyboardHook.dll` 的 `HookData`（RVA 0x33000）**不是共享节**，跨进程看不到任何句柄——`KillHook` 只有在"远志把这个 DLL 注入进了当前进程"时才有可能有效。
   * `ExdHooks.dll` 的 `.ExdHook`（RVA 0x7000，56 字节）是**真共享节**，其中 0x70000 / 0x70004 / 0x70008 三个字段是三支钩子的 HHOOK。`UnSetExdHooks(0)` 的流程是：`EnumWindows` 回调里对每个顶层窗口 `RemovePropA(hwnd, atom)`（atom 取自**私有**全局 0x6418，在我们进程里是 0），然后对那三个字段逐个 `UnhookWindowsHookEx` 并清零；`UnSetExdHooks2(tid)` 要求传入 tid 等于共享节里记录的安装线程 tid，否则直接返回 0（`KillHook` 无参，`UnSetExdHooks`/`2` 各带 1 个 DWORD，与最初计划书"全部无参"的假设不同）。
   * 机房实测 `UnSetExdHooks` 返回 1 → 它走到的是"三个字段都是 0"的分支，即 **ExdHooks 这一层当时没有装钩子**；`UnSetExdHooks2` 返回 0 → tid 不匹配，符合预期（钩子不是我们这个进程装的）。**结论：这条路线目前没有证据支持，别再重复投入。**
   * 导出签名不符时会被运行时自检拒绝（记 ERROR、功能不生效，不影响其它功能）——这层保护仍然有效。
8. 外部窗口纠正要过 win32k 的权限检查（**UIPI**）。09-23 管理员身份下实测 `写样式`/`写扩展样式`/`定位` 三处全 `err=0x5`、30 次全败；**09-29 换成 SYSTEM（与客户端同级）后同一台机器上立刻成功**（见"2026-09-29 判决"）。所以这条路径**可用，但必须用 SYSTEM 身份启动主程序**。管理员身份下仍会记 WARN、状态栏显示"有写入失败"；本地模拟目标与主程序同完整性，必然成功，不能用来判断这条路径。
   **反横跳闸门（v0.6.6）**：跨进程纠正没法抑制客户端的抢回动作，若客户端**响应我们的改动**立刻变回无边框全屏，硬顶下去就是 Win11 下"放大缩小来回横跳"。所以按**间隔**判定打架（连续 3 次在 1.5 秒内被抢回），判定后对该窗口**停手**并记一条 WARN（含"请改用 SYSTEM + InjectMethod=1"的指引）；停手记录 5 分钟后或窗口销毁时丢弃。注意：远志自己"每 3 秒抢一次"的慢节奏**不算打架**，那种情况仍按每秒一次持续纠正（设计如此）。想要真正压住高频抢回，只有进程内 Hook 这条路。
   **键鼠封锁在设备层**：客户端进程内没有任何 `ClipCursor`/`BlockInput` 调用，封锁由 `Yistart.exe` 每 2~3 秒拉起的 `KillMain.exe -d:set` 维持；该 exe 只导入 `DeviceIoControl`/`CreateFileW`/`WriteFile`，没有任何用户态输入 API。所以屏幕被钳在左上角、键盘被禁都与我们的 hook 无关，也**不可能**用用户态 hook 解开。若出现这种情况，正确判断是"这是设备层封锁，不是工具没生效"。
9. 外部窗口纠正依赖与注入器相同的结构判据，因此**广播窗口若带标题栏或既非 POPUP 也非置顶就会漏判**。2026-09-23 已经有了广播期的窗口证据：**全屏广播**下是 `Brw:…` 类、标题"屏幕广播系统"、置顶、无边框、不覆盖整屏；**窗口广播**下同一个类名的窗口处于**最小化**状态（style 含 `WS_MINIMIZE=0x20000000`，rect 为 -32000）。也就是说远志的广播窗口类名以 `Brw:` 开头，"覆盖整屏 + 无标题栏"只在其全屏形态下成立；要不要按类名前缀放宽判据，等下一次在**全屏广播中**同时跑主程序再定，现在改是盲改。
10. 假全屏只是"看起来还是全屏"：它**不隐藏屏幕内容**，教师在抓屏/监视时看到的仍是你真实屏幕；而且窗口尺寸与置顶状态是远志有权查询的属性，不能排除教师端据此判断异常。它的定位是配合防监视使用，不是反检测手段。另外假全屏下 `YZ_FLAG_TOPMOST`（窗口置顶开关）不生效——这个模式的全部意义就是不置顶。
11. 远程执行审计只看学生端进程内的用户态 API 调用：注入不进去时没有审计；教师端若通过内核驱动/服务通道直接下发动作（不经 `CreateProcess` 等），也记录不到。它是**审计**而不是防护，日志位置 `%TEMP%\YZTrainer\remote-exec.log`（导出诊断包时会一并复制进 `diag-<时间戳>\`）。
12. **策略项（HKCU）的 hive 归属——已有本机实测证据，批次 2 待上机确认**：`policy.cpp` 里全部走 `HKEY_CURRENT_USER`，而这段代码跑在**目标进程**里（`Yistart.exe` 以 SYSTEM 运行时就是 SYSTEM 的 HKCU），跟登录学生那一家不是同一个。2026-09-23 本机实测（令牌复制方式启动、会话 1 的 SYSTEM 进程）：
    * `RegOpenCurrentUser` + `NtQueryKey` 自报的 HKCU 是 `\REGISTRY\USER\.DEFAULT`，**不是** `S-1-5-18`；
    * 但我们 `PolicyEnforce` 写下的值**同时**出现在 `HKU\.DEFAULT` 与 `HKU\S-1-5-18` 下，两处 `Environment` 内容逐字相同 → 这台机器上这两个名字是**同一个 hive**。
    所以：外部只看 `S-1-5-18` 会得出"根本没写过策略"的错误结论。**v0.6.4 起注入侧自己会报一行 `PolicyHive: 本进程 HKCU → …`**（只读、不改状态），现场以它为准；确认前不引入 `WTSQueryUserToken`/跨 hive 穿透逻辑（注入本身还没稳，不想再加一层不确定性）。**v0.6.13 起这段代码只做只读自报，不再写任何策略值**（见第 15 条）。
13. **消息层钩子的处理是“可二分的风险点”（v0.6.13 起不必再重编，v0.6.14 修好并加了回归）**：`ShouldBlockHook` 也会把 `WH_GETMESSAGE` / `WH_CALLWNDPROC` 换成空过程（远志的 `KeyboardHook.dll` 导出了 `SetMessageCallback`/`SetWndProcCallback`，这两类钩子可能是锁键鼠的载体，但也可能是广播消息链的一环）。现场若出现“广播画面不动/窗口消息异常”，把 ini 里的 `BlockMessageHooks` 改成 `0` 重启程序即可做对照——v0.6.13 把这个开关从编译期宏改成了配置位（`YZ_CFG_BLOCK_MESSAGE_HOOKS`）。**注意 v0.6.13 那版这个开关其实是失效的**（控制位被 `YZ_FLAG_FUNCTION_MASK` 剔除，钩子侧读 `g_flags` 永远读到 0，等于悄悄丢了原来的默认行为；另外 `ConfigLoad` 里"读 ini 值"排在"同步控制位"之后，改成 0 也不生效），v0.6.14 两处都修好并新增回归阶段九把两半都钉住。这两类在日志里单独记成 `已用空过程替换远志消息层钩子(W/A)`，便于定位。
14. **键鼠封锁：2026-10-09 终结实验，正式结案（用户态无解）**。三条证据：① 整段广播期客户端**一次都没调用** `ClipCursor` / `BlockInput` / SPI 改写 / 抢注热键（这些拦截点命中即打印，计数全为 0）；② 内核 `clip` 矩形全程 `0,0,1920,1080`（整屏），而光标在广播期间约 90% 采样停在 `(0,0)`，广播一结束立刻恢复自由；③ 客户端在 09:01:15.427 尝试安装自己的 `WH_MOUSE_LL`（idHook=14），被我们成功拦下，之后 38 分钟鼠标照锁不误。再加上封锁由 `Yistart.exe` 每 2~3 秒拉起的 `KillMain.exe -d:set` 维持（该 exe 只导入 `DeviceIoControl`/`CreateFileW`/`WriteFile`/`TerminateProcess`，没有任何用户态输入 API）→ **封锁在驱动/设备层，用户态（含进程内 hook）不可能解开**。判读提醒：`ClipCursor 探针` 的 `clip` 读数会被我们自己的钩子影响（`Hook_ClipCursor` 命中时会把钳制清零成全屏），所以看读数前先确认日志里 `已解除鼠标区域锁定(ClipCursor)` 的计数——**计数 >0 时该读数不构成证据**。

**2026-10-14 状态更新**：用户要求在世的前提下复核并**定位拦截层**，范围严格限定在**用户态**（不写内核驱动、不碰厂商驱动文件——硬边界不变）。为此新增 `tools\input-probe.ps1`：A `SendInput`、B `PostMessage`、C 自装 `WH_KEYBOARD_LL`（**合成/物理事件分开计数** + 自校准 + 要求人工确认"确实按了物理键"）、D **只读**驱动栈侦察（键盘/鼠标/HID 类的 Upper/LowerFilters 与运行中的相关内核驱动）。判读表：A 通 ⇒ 只卡在目标程序的用户态钩子（拔钩路径可解）；A 不通、B 通 ⇒ 队列被拦但目标认消息；A 不通而 C 的**物理**计数仍 >0 ⇒ 拦截在用户态队列过滤层；三者都不通且自校准通过、人工确认按过 ⇒ 仍是**设备/驱动级**，则按上面三条证据的结论回报，不越界。
15. **策略项（HKCU）在机房机器上的实测结论 —— 已据此删除写入逻辑（v0.6.13）**：学生 hive（`S-1-5-21-…-500`）里**一条策略值都没有**；有值的只有 `.DEFAULT`/`S-1-5-18`（同一 hive）——而那正是我们自己的 `PolicyEnforce` 写进去的（注入侧自报 `PolicyHive: 本进程 HKCU → \REGISTRY\USER\.DEFAULT`）。也就是说：远志的封锁**不走 HKCU 策略**，我们那套策略读写既没作用在正确位置、也没必要。**"批次 2（改主程序侧跨 hive 修策略）"早已取消**；v0.6.13 又把 `policy.cpp` 里的 `PolicyBackup`/`PolicyEnforce`/`PolicyRestore` 连同那张策略项表**整段删掉**，只保留 `PolicyLogCurrentUserHive()` 这行只读自报。留着的副作用是"把几个 0/1 写进默认用户模板"（`.DEFAULT` 会被之后新建的 profile 继承，而进程被硬杀时"退出还原"根本不会执行），收益为零——所以不再"等下次动代码时再决定"。

## 网管版（GZYZ）适配

机房里实测部署的是**网管版**，与普通学生端有几处差异，代码里已按下面规则处理：

* **安装前缀**：网管版默认装在 `C:\Program Files (x86)\GZYZ\YZinfo Multimedia teaching softwareV9.0 Student\`。所有"是否远志"的路径判据统一走 `yz::IsYuanzhiInstallPath()`，同时认 `YZinfo Multimedia teaching software` 与 `GZYZ` 两种前缀——注入器的目标打分、Hook 侧的宿主识别、考试模式的窗口归属判据都改用它，避免只认一种前缀造成漏判。探针的服务/模块过滤关键词本来就含 `GZYZ`。
* **守卫进程 Nmdeputy.exe**：服务以 `Nmdeputy.exe /NormalApp /GlobalMutex /StopParam:stop /Delay:1000 /Delay2:3000 /DelayInit:3000 /SelfGuard /Protection /Name:… Service /"…\Yistart.exe" -AutoUninstall` 的形式拉起客户端。它是**守护与上报**进程，因此：**注入器显式排除它**（`IsGuardProcess`，日志里提示一次"检测到网管版守卫进程"，只注入客户端）；同时正因为它会重启/重新保护客户端，看门狗每 2 秒补注入这条机制在网管版上是必需的，不能省。
* **目标优先级**：广播窗口宿主(+5) ＞ 主进程优先（`Yistart.exe` +3、`ExdPaintHelper.exe` +2、`TEACHCMD.exe`/`PlayerGUI.exe` +1）＞ 进程名在名单(+3) ＞ 安装路径匹配(+2)，守卫进程永不入选。这样即便名单里同时有多个客户端进程，也会稳定落到 `Yistart.exe` 或真正的广播窗口宿主上。
* **客户端被定向剥夺句柄权限（2026-09-18 两台机房机器实测）**：`Yistart.exe` 的进程保护级别是**无保护**（`0xFFFFFFFE`，即不是 PPL），而 `Nmdeputy.exe` 可以正常枚举模块、`VirtualAllocEx`/`WriteProcessMemory` 全部成功——说明保护是**只针对客户端进程**的句柄权限剥夺（`ObRegisterCallbacks` 一类），不是进程保护机制。典型现象：注入时报 `VirtualAllocEx 失败 0x00000005`，连模块都枚举不到。首选免注入路线（`YZUnhookTest`），或把 `InjectMethod` 切成 1 试消息钩子。
* **窗口化不依赖注入（v0.4.0 起）**：正因为上面这条，**广播窗口化改走双路**——进程内 Hook 拿不到目标进程时，主程序用跨进程 `SetWindowLongPtrW`/`SetWindowPos` 直接改那个窗口（`ExternalWindowFix=1` 默认开）。跨进程改样式是会话级操作，不需要目标进程配合，但需要过 win32k 的完整性检查（目标以 SYSTEM 运行、我们以管理员运行时是否放行，以机房实测为准，日志会记 WARN）。
* **首要怀疑对象：`ProcessProtect.sys`**（2026-09-23 三台学生机交叉比对得出）：`C:\Windows\system32\Drivers\ProcessProtect.sys`，无厂商、无文件版本，在 `YZ-26.9.23-Mine`、`YZ-26.9.23-ClassmateA` 与 09-18 的学生机报告里**都出现**，而**教师机没有**——"保护只针对学生端"这件事与它的名字都对得上。名字相似的 `pkdsk` / `pkprot` / `pkdhlp` 教师机同样存在，可以排除；`BrDevfer`（`BRUsbFilter.sys`）降为次要候选。下次上机要做的是：用新版探针的逐权限位表确认它是否在有人打开 `Yistart.exe` 句柄时削减权限，并补记它的签名者与文件版本。
* **不要再用“完整性级别差”解释 0x5**：同日实测里 `Nmdeputy.exe` 与学生端同为 SYSTEM / 系统完整性(16384)，而探针以高完整性(12288)对它的模块枚举、`ReadProcessMemory`、`VirtualAllocEx`、`WriteProcessMemory` **全部成功**。若是 `UIPI` / 强制完整性策略的写升拦截，Nmdeputy 也必须失败。所以 `VirtualAllocEx 0x5` 只能来自针对**特定进程**的句柄权限剥夺（内核回调），而不是级别不够——这条同时意味着"提到 SYSTEM 就能过"并不必然成立，必须以新版探针在 SYSTEM 身份下的复测为准。
* **打包提醒**：`build.ps1` 会在 `dist\<平台>\` 写入一份出厂默认 `YZTrainer.ini`（`Flags=5`、`LogLevel=2`、`EnableExamGuard=1`、`ExternalWindowFix=1`、`InjectMethod=0`、`AutoExportOnCapture=1`、`BlockMessageHooks=1`），避免把调试配置（例如打开防监视的 `Flags=13`）随压缩包发出去。

### 为什么注入会失败（`VirtualAllocEx` 返回 0x5）

常规注入是四步，我们卡在第二步：

1. `OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | ...)` 拿到目标进程句柄；
2. **`VirtualAllocEx`**——在**目标进程的地址空间**里申请一块内存（我们要用它存 DLL 的路径字符串）；
3. `WriteProcessMemory` 把 DLL 路径写进那块内存；
4. `CreateRemoteThread(LoadLibraryW, 那块内存)` 让目标进程自己把 DLL 加载进去。

`VirtualAllocEx` 需要句柄带 `PROCESS_VM_OPERATION`。现在的情况是：`OpenProcess` **成功**（DACL 检查通过、`SeDebugPrivilege` 也开了），但拿到的句柄其 `GrantedAccess` 被**事后削减**——驱动用 `ObRegisterCallbacks` 注册进程回调，在每次有人打开受保护进程的句柄时剥掉 `PROCESS_VM_OPERATION`/`PROCESS_VM_WRITE`/`PROCESS_VM_READ`（可能还有 `PROCESS_CREATE_THREAD`）。于是第二步在对象管理器里被判 `STATUS_ACCESS_DENIED`，`GetLastError()` 得到 5。同一个原因也解释了为什么连模块枚举（内部要 `PROCESS_VM_READ`）都失败。

这**不是权限不够**：已经是管理员 + `SeDebugPrivilege`，提权再多也没用（`SeDebug` 绕的是 DACL，不绕句柄权限削减）。

2026-09-23 在教师真实全屏广播期间实测（`YZ-26.9.23-Mine` 的两份 diag），**四条路全都不通**：

| 路线 | 实测结果 |
|---|---|
| 远程线程注入（`InjectMethod=0`） | 重试 10 次，全部 `VirtualAllocEx 失败 0x5` |
| 消息钩子注入（`InjectMethod=1`） | `SetWindowsHookEx` 返回了 HHOOK，但 DLL 只进了**我们自己**的进程（日志里那句"宿主进程不是远志学生端"就是它），目标进程始终没连上管道：`连接=0 Hook数=0` |
| 跨进程窗口化（`ExternalWindowFix`） | 找到候选广播窗口后，三处写操作全部 `err=0x5`，30 次 |
| 免注入拔钩（`YZUnhookTest --apply`） | 三个入口都"调用成功"，但键鼠没有恢复 |

其中一条判读经验：**`SetWindowsHookEx` 的返回值不能当作注入成功的证据**。系统是延迟映射 DLL 的，映射失败（被剥夺权限或被 `UIPI` 挡下）时不会回头报错。唯一可信的判据是"目标进程有没有连上管道、`Hook数` 是不是 0"。

四条负面结论指向同一个机制（内核回调剥夺 `Yistart.exe` 的句柄权限），`ProcessProtect.sys` 是当前唯一符合"只在学生机上出现"的候选驱动。

2026-09-23 下午补齐了**身份对照实验**（用 `YZSysRun.exe` 在交互会话里以 SYSTEM 跑探针，档案见 `archive/YZ-26.9.23Afternoon-Mine/`）：管理员(12288) 与 SYSTEM(16384) 两份报告的"注入能力实测"**逐字段相同**——`Yistart.exe` 的 `GrantedAccess` 都是 `0x00001402` = `PROCESS_QUERY_LIMITED_INFORMATION`(0x1000) | `PROCESS_QUERY_INFORMATION`(0x0400) | `PROCESS_CREATE_THREAD`(0x0002)，被削掉的正是 `PROCESS_VM_READ` / `PROCESS_VM_WRITE` / `PROCESS_VM_OPERATION` 这三位（`0x1402 + 0x38 = 0x143A`，与同机不受保护的 `Nmdeputy.exe` 拿到的完整掩码刚好差这三位）。**结论：保护是按"目标进程"判定的，与调用者身份无关**——提到 SYSTEM 没有意义，基于 `VirtualAllocEx` 的远程线程注入路线可以正式判定为不可行。

同时留了两条残余能力，值得记着但别高估：`PROCESS_CREATE_THREAD` 与 `PROCESS_DUP_HANDLE` 没被削，目标**线程**句柄也是完整的（`OpenThread` 实得 `0x0000185A`，含 `THREAD_SET_CONTEXT|THREAD_SUSPEND_RESUME`）——说明驱动只给 `PsProcessType` 注册了回调、漏了 `PsThreadType`。但"线程劫持"落地时仍要把字符串/参数写进目标内存，而 `PROCESS_VM_WRITE` 恰好在被削的那三位里，所以它不是免费的替代路线。

### 2026-09-29 判决：两条路都通了（这才是关键结论）

在机房同一台机器、同一个客户端上做了身份对照（`archive/YZ-26.9.29/`）：

| 启动方式 | 跨进程改窗口（`ExternalWindowFix`） | 消息钩子注入目标进程 |
|---|---|---|
| 管理员（高完整性 12288） | 写样式/定位**失败 err=0x5**，30 次 | 只进了自己进程，目标没连管道 |
| **`YZSysRun` 以 SYSTEM（系统完整性 16384）** | **成功**：`外部窗口纠正: 0x000206BC -> 384,216 1152x648` | **成功**：`[Hook pid=7940] Hook 安装完成，共 29 个`，管道连通、配置下发 |

所以模型要修正为**两种机制并存**：

1. **句柄 VM 权限剥夺**（内核组件，`ProcessProtect.sys` 一类）——按**目标进程**判定、与调用者身份无关，
   所以 `VirtualAllocEx` 远程线程注入在任何身份下都不可能成功；
2. **UIPI 一致性检查**（win32k）——只管 `SetWindowsHookEx` 与跨进程窗口写，**我们与目标同级（SYSTEM）时放行**。
   09-23 把这个 0x5 归因成"驱动剥夺"是**错的**，那是 UIPI。

**结论**：受保护客户端上，"窗口化/假全屏 + 消息钩子注入"这条组合路线**是可行的**，前提是用 SYSTEM 启动主程序
（v0.6.5 起程序会自动提示并运行期兜底改用消息钩子）。

**键鼠解锁同样结案（2026-10-09 终结实验）**：客户端进程内没有任何 `ClipCursor`/`BlockInput` 调用，内核 clip 矩形整段广播期都是整屏而光标仍被拽回左上角，且我们拦下它的 `WH_MOUSE_LL` 安装后鼠标照锁不误——封锁由 `Yistart.exe` 每 2~3 秒拉起的 `KillMain.exe -d:set` 维持，发生在驱动/设备层；用户态 hook（哪怕已经装进客户端 29 个）对它无效。要动它就得碰驱动或干掉厂商进程，两者都在项目硬边界之外，因此 v1 的键鼠解锁标记为**未解决**，不再投入。

### 2026-10-10 机房验证：客户端重启循环消失、键鼠结案、采集层定性

四件事在这一轮同时收口（档案 `archive/YZ-26.10.9/`、`archive/YZ-26.10.10/`）：

1. **窗口化在真机首次完整成立**（10-09）：`广播窗口已窗口化: 0x002A0D58 -> 384,216 1152x648`，此后 39 分钟稳定在 60%，全程没有横跳、没有“认输停手”；外部探针独立复核到同一个 `Brw:` 窗口（style `0x14CF0000`、置顶=否、覆盖整屏=否）。
2. **v0.6.11 的“空过程替换”在真机验证通过**（10-10）：`已用空过程替换远志键盘/鼠标钩子(A): idHook=13 … -> hhook=0x000C06F7`、`idHook=14 … -> hhook=0x000304E8`，拿到的是**合法 HHOOK**；替换后客户端进程一直活着（没有 `Hook DLL 连接结束`、没有十秒级重启），窗口照常窗口化。对照 v0.6.10 的现场日志：16 次 `已拦截…安装`，每次都紧跟着 5~8 毫秒后进程消失。
3. **键鼠解锁正式结案（用户态无解）**：见“已知限制”第 14 条的三条证据；判读 `ClipCursor 探针` 前要先确认 `已解除鼠标区域锁定(ClipCursor)` 计数为 0，否则那个读数是我们自己造成的。
4. **采集层定性（静态）**：`KsFiles\ExdDtDup.dll` 是 DXGI 桌面复制实现（见“已知限制”第 1 条）；运行时“谁在抓屏时加载它”仍待另采一次，YZProbe 的“图形采集栈”行就是为这件事准备的。

### 2026-10-11 本地静态侦察 + 采集栈取证链（v0.6.12）

这一轮没有上机，做的是**下一趟机房的前置**：冻结画面（防监视）要钩哪条路，取决于"教师监视时谁在抓屏"，
而这件事静态只能定一半、运行时只能靠现场那几秒——所以先把两件事做好：把静态那半搞清楚、把运行时那半变成自动采集。

1. **反汇编 `ExdDtDup.dll`（本机只读副本）**：确认它走 `IDXGIOutput1::DuplicateOutput`（槽 22 / 偏移 `0x58`）
   → `IDXGIOutputDuplication::AcquireNextFrame`（槽 8 / 偏移 `0x20`）；**光标是它自己合成的**
   （`GetCursorInfo`/`GetIconInfo`/`GetObjectW`，与取帧同一个循环）；帧经共享节交给消费端；
   每块显示器一个采集线程 + 一个 Duplication 对象；`DuplicateOutput` 失败时会区分
   `DXGI_ERROR_NOT_CURRENTLY_AVAILABLE`（别人已在抓屏）。这些决定了冻结画面的两条实现路线（见已知限制 1）。
2. **发现采集层其实是两条实现**：除 DXGI 这条，还有 GDI 的 `ScreenRecoder.exe`（独立进程，被
   `Rmdesk.ads`/`CmdProc.ads`/`ProxyM.exe` 引用）。也就是说"教师监视"未必走 DXGI——如果走 GDI，
   真正的缺口是"冻结钩子不在那个 helper 进程里"，比写 DXGI vtable 钩子便宜得多。
3. **探针三处口径修正**：对**全部进程**轻量扫 `ExdDtDup/ExdElite/ExdPaint`（避免漏掉"看起来不像远志"的宿主）；
   区分"模块枚举失败"与"确实没加载"；磁盘有 `ExdDtDup.dll` 的判词从"采集层=DXGI"降级为
   "本机装着这个实现，但本次没观察到它被加载"。
4. **新增两条常驻自动采集**（都不需要人在现场守着那一刻）：
   * 探针 `YZProbe.exe --watch`：触发式落盘（新远志进程 / 采集组件加载 / 疑似广播窗口），每分钟一条心跳；
   * 主程序每 30 秒扫一轮，首次发现采集组件即记日志 + 静默导出诊断包，且每份诊断包的 `snapshot.txt`
     都带 `== 采集栈快照 ==`。
5. **本地回归 13 → 15 项**：新增阶段七，用改名的 `version.dll` 冒充 `ExdDtDup.dll` 载入模拟目标，
   断言"常驻监控发现 + 诊断包快照含命中"。实测 **15 项全 PASS、失败项 0**
   （`build\logs\sim-test-v0612.log`、`sim-test-v0612-final.log`）。
   如实标注：真实 `ExdDtDup.dll` 在本机普通进程里**载不进去**（x64 报 193 位数不符、x86 报 127 缺导出），
   所以本地用的是改名替身；检测逻辑只看模块名，这条验证有效，但"真机上由谁加载"仍只能机房实测。

### 2026-10-11（第二轮）v0.6.13：P0 修复补上回归闸门 + 三项清理

第一轮把"取证链"做好之后，这一轮做清单里的第 2 项（本地防线）与 3/4/5 三项清理。

1. **阶段八：P0"空过程替换"的回归闸门**。v0.6.11 那条修复此前只有真机验证、没有任何本地防线
   （模拟目标对 `SetWindowsHookEx` 的返回值根本不在乎）。现在给 `YZSimTarget` 加了
   `--hook` / `--hook-thread`：像厂商那样自己装键盘钩子，并**把返回 `NULL` 当生死判据直接退出**
   （复现现场"8 毫秒内进程消失"）；回归脚本把模拟目标复制到**路径含 `GZYZ`** 的目录里跑，
   命中 `IsYuanzhiInstallPath()` 的子串判据——**不需要在生产代码里开任何后门**。
   注意 `--hook` 是**启动 8 秒后**才装钩子：现场那次客户端也是"Trainer 已经在跑之后"才装的；
   启动时立刻装会走系统原生路径、完全绕开我们的钩子（第一版就这么写的，实测两边都不命中）。
2. **负向 A/B：证明这条闸门真的会拦人**。一次编排里跑三段：
   正常代码 **17 项全 PASS**；把 `ReplaceWithPassthrough` 临时改成 `return nullptr`（= v0.6.10 行为）
   后 **15 PASS / 2 FAIL，红的正是阶段八那两条**；撤掉补丁重编后又是 **17 PASS / 0 FAIL**
   （`build\logs\sim-test-v0613.log`、`-ab-null.log`、`-final.log`）。
3. **顺手修掉回归脚本自己的两个真问题**：① 阶段八的句柄正则只认带 `0x` 前缀的格式，
   而模拟目标用 `%p` 打印（`hhook=000B0E84`）——改用 `hhook=(?:0x)?0*[1-9A-Fa-f]`；
   ② 断言原来直接 grep 整个 `yzt.log`，而它是追加写的、跨次运行共用，**会被上一次运行的旧行满足**
   （负向 A/B 里差点变成恒绿）。现在脚本用 `Get-LogLength`/`Read-LogTail` 只匹配本阶段新增的那段。
4. **清理项 3（注入体验）**：注入前先读一次目标句柄的实得权限（`NtQueryObject`，结构体必须恰好 56 字节），
   缺 `PROCESS_VM_OPERATION` 就直接跳过注定失败的远程线程注入、改走消息钩子，
   不再白试 3 次/约 5 秒/3 条 WRN。同时收紧自动改道的前提：本程序完整性**低于**目标时不改道
   （那种情况下消息钩子同样被 UIPI 挡下），只记一条"请用 YZSysRun 以 SYSTEM 启动"。
   `InjectMethod` 出厂默认**仍是 0**——消息钩子会把 DLL 映射进本会话所有 GUI 进程，默认开的面太大。
5. **清理项 5（消息层钩子开关）**：原来的编译期宏 `YZ_BLOCK_MESSAGE_HOOKS` 改成配置位
   `YZ_CFG_BLOCK_MESSAGE_HOOKS` + ini 项 `BlockMessageHooks`（默认 1）。现场要二分
   "消息层钩子是不是广播链一环"时改 ini 重启即可，不必改代码重编、重发产物。
6. **清理项 4（HKCU 策略写入）**：`PolicyBackup` / `PolicyEnforce` / `PolicyRestore` 与那张策略表
   **整段删除**，只留只读的 `PolicyHive: 本进程 HKCU → …` 自报。理由见已知限制 12/15：
   机房实测远志的封锁不走 HKCU 策略，而那套写入落在**默认用户模板**（`.DEFAULT`）上，
   进程被硬杀时"退出还原"根本不执行——收益为零、副作用是真的。

### 2026-10-11（第三轮）v0.6.14：把上一轮的行为变更钉进回归，并因此抓出两个真 bug

上一轮（v0.6.13）改了两处**行为**（注入预检、消息层钩子开关），但当时只有"编译通过 + 原有 17 项仍绿"。
这一轮补上覆盖（阶段一 +2 条、新增阶段九 2 条，共 **21 项**），结果**第一次跑就抓出两个真 bug，
而且都是 v0.6.13 引入的**：

1. **消息层钩子开关在钩子侧永远读到 0**：`ShouldBlockMessageHooks()` 读的是 `g_flags`，
   而 `ApplyEffectiveFlags()` 把 `g_flags` 限制成 `g_configFlags & YZ_FLAG_FUNCTION_MASK`——
   控制位**被有意剔除**（那是给考试守护位设计的正确约束）。后果：从 v0.6.13 起
   `WH_GETMESSAGE`/`WH_CALLWNDPROC` 再没被换成空过程，等于悄悄丢掉了原来那个宏的默认行为。
   修法：新增独立变量 `g_blockMessageHooks`（默认 1），配置下发时按控制位赋值。
2. **ini 改成 0 也不生效**：`ConfigLoad` 里 `SyncBlockMessageHooksBit(cfg)` 排在读
   `BlockMessageHooks` **之前**，同步时用的还是默认 true。修法：把同步挪到读值之后，
   并在代码里写明"新增控制位时顺序不能反：读值 → 同步位"。

三次跑的对照本身就是证据：第 1 次 20 PASS/1 FAIL（开关恒 0 → 默认那一半没替换）、
第 2 次 20 PASS/1 FAIL（位对了但 ini=0 不生效）、第 3 次 **21 PASS/0 FAIL**（`sim-test-v0614.log`）。
这也是"把行为固化成断言"最直接的收益：这两个 bug 都不会被编译器和代码审查拦下，
只有让回归去问"开关为 0 时到底发生了什么"才会暴露。

### 2026-10-11（第四轮）v0.6.15：探针 --watch 与 hMod=NULL 分支也进回归（21 → 25 项）

这一轮补的是两处**此前从未被任何测试覆盖**的路径：

1. **阶段十：探针 `--watch`**（新增 2 项）。现场"教师监视时谁在抓屏"有两条自动采集通道，
   阶段七只覆盖了主程序那一半；探针是随身工具，它坏了那一趟机房基本白跑。
   断言：常驻探针 + 让某进程加载改名成 `ExdDtDup.dll` 的替身 → 必须落一份 `-watch-capture`
   报告且内容命中；并按 `--duration` 正常退出（watch 日志以"watch 结束：共落盘 N 份报告"收尾）。
2. **阶段八第二段：`hMod=NULL` 的线程内钩子**（新增 2 项）。`ShouldBlockHook` 的两条归属判据里，
   "按回调地址反查模块"那条（线程内钩子）此前从未被走到过；现在用 `YZSimTarget --hook-thread`
   装一个 `SetWindowsHookExW(WH_KEYBOARD, ..., nullptr, 本线程 tid)` 把它覆盖住。

**这一轮又踩了一个测试侧的坑**：第一版把第二段写在前一段的 `try` 里面，导致前一段的模拟目标
在主程序被杀之后仍然活着（管道已断、不会被补注入），新主程序的看门狗就反复给它补注入
（日志里同一个 pid 被"已注入目标进程 (第 2..10 次)"刷了十次），而**真正的新目标永远等不到注入**，
它的钩子走原生路径安装 → 断言误报失败。修法是把第二段搬出去、每段独立闭环（起目标 → 起主程序
→ 断言 → 清理）。实测：第一次 24 PASS/1 FAIL（红的正是这条，原因在脚本而非产品），
修好结构后 **25 PASS/0 FAIL**（`sim-test-v0615.log`）。

### 2026-10-11（第五轮）v0.6.16：考试熔断进回归——补上压在硬边界上的最后一个缺口（25 → 27 项）

翻了一遍 25 项断言的覆盖，发现**考试熔断是唯一压在项目硬边界上、却一条断言都没有的行为**
（AGENTS 写明"考试/测验强信号命中即全功能停用"，`EnableExamGuard=0` 只许调试、不得作为交付默认）。
更要紧的是：v0.6.13 与 v0.6.14 连着两个 bug **都出在"控制位下发"这条链上**，而考试守护位走的
正是同一条链——它一旦失效，等于硬边界被悄悄松开。

* **阶段十一（新增 2 项）**：把模拟目标放进**路径含 GZYZ** 的目录（强信号 3 要求"标题命中考试
  关键词的可见窗口"且窗口所属进程在远志路径下）并用 `--exam` 造出那个窗口，主程序照常注入。
  * 断言 26：`EnableExamGuard=1` 时日志出现 `检测到考试/测验模式(考试)，已停用全部功能`，
    **并且功能真的停了**——目标状态仍是 `fullscreen=1`（没有被窗口化）。"有日志"不够，
    必须看到可观测的效果，否则"只打日志不干事"也能骗过断言。
  * 断言 27：对照组 `EnableExamGuard=0` 时，钩子侧必须记录
    `考试模式检测已按配置关闭(EnableExamGuard=0)`，且功能恢复（`fullscreen=0`）。

**顺带纠正了一个文档级误解**：对照组的断言第一版写成"整段日志不得出现熔断行"，实测红了——
原因是钩子侧 `g_examGuard` 初值就是**开**（fail-closed），注入到配置到达之间约 1 秒内即使 ini
写着 0 也会先熔断一次，随后才解除。**这是设计行为**（宁可误熔断不可漏熔断），
但它意味着：现场读 `EnableExamGuard=0` 的日志时，早期出现一条熔断记录属正常，
判据是**后面有没有"考试模式检测已按配置关闭"**。这条已补进 README 的考试模式实现要点。
实测：第一次 26 PASS/1 FAIL（红的正是对照组口径），修好断言口径后 **27 PASS/0 FAIL**
（`sim-test-v0616.log`）。

### 2026-10-11（第六轮）v0.6.17：YZSysRun 令牌自检进回归（27 → 29 项）

`YZSysRun.exe` 是**现场部署的强制启动路径**（学生端跑 SYSTEM，主程序必须同级完整性，
否则注入、消息钩子、跨进程窗口写三条路全被 UIPI 挡下），README 与 `工作顺序.txt` 都把它
写成第一条硬要求，但回归里一条断言都没有。它有两种都是致命的坏法：令牌复制失败 → 子进程
根本不是 SYSTEM，现场白跑；子进程落到 **Session 0** → 探针看不到窗口、挂不上消息队列，
采回来的数据全部失真（README 里专门写过"会话号与当前不一致时那张窗口表是空的，别用"）。

**阶段十二（新增 2 项）**：用它起一个 `cmd.exe` 子进程并把输出写进指定 txt，断言只匹配
ASCII 片段与数字（与输出文件编码无关）——① 令牌是 `NT AUTHORITY\SYSTEM`/系统完整性；
② `已启动 … 会话=<当前会话> … 16384`，会话号直接与 `(Get-Process -Id $PID).SessionId` 比较。
本机实测的输出原文：

```
本进程 : pid=25708 会话=1 完整性=高(12288)
令牌来源: winlogon.exe pid=1164（会话 1）
复制令牌: 用户=NT AUTHORITY\SYSTEM 会话=1 完整性=系统(16384)
已启动 (CreateProcessWithTokenW): pid=13228 会话=1 完整性=系统(16384)
子进程退出码 = 0
```

一次通过，**29 PASS / 0 FAIL**（`sim-test-v0617.log`）。到这里现场四件套各自都有断言守着：
主程序（注入/窗口/输入/审计/采集栈/考试熔断）、探针（图形采集栈 + `--watch`）、
`YZSysRun`（令牌与会话）、以及回归脚本自身（负向 A/B 证明闸门会拦人）；
`YZUnhookTest` 仍是对照组工具，不设断言（结构上已证不成立）。
### 2026-10-11（第七轮）v0.6.18：免注入路径的"假全屏"进回归（29 → 31 项）

窗口层有三条路径，此前覆盖了两条：阶段三测**进程内**假全屏（`AutoInject=1`）、阶段二测跨进程
**窗口化**；缺的正是两者的组合——**注入不进去时靠跨进程纠正做假全屏**。而机房"客户端被剥句柄
权限、`VirtualAllocEx` 返回 0x5"时，`ExternalWindowFix` 就是唯一还能生效的窗口手段，
README 明确写了它同样支持假全屏，但此前一条断言都没有。

**阶段十三（新增 2 项）**：`Flags=36` + `AutoInject=0` + `ExternalWindowFix=1`，
断言 ① 状态文件出现 `fullscreen=1` 且 `topmost=0`，日志有 `外部窗口纠正(假全屏): …`；
② 窗口没有被加标题栏（外观仍是"全屏广播"）。一次通过，**31 PASS / 0 FAIL**
（`sim-test-v0618.log`）。

至此窗口层四条路径（进程内/跨进程 × 窗口化/假全屏）、输入层两条归属判据 + 消息层开关两半、
采集层（GDI 冻结 + 采集栈监控 + 探针 `--watch`）、审计与卸载、以及身份相关的三条
（注入预检、考试熔断、`YZSysRun` 令牌会话）全部有断言守着；没被断言的只剩 `LogLevel`、
日志滚动、`AutoExportOn*` 置 0 这类低价值项。

### 2026-10-11（第八轮）v0.6.19：身份不足时的判读进回归（31 → 33 项）

这是本项目历史上**最费时间的坑**：09-23 把跨进程窗口写的 `err=0x5` 判成"内核驱动剥夺句柄权限"，
于是"换身份也没用"的结论被写进交接文档，后续 session 白跑；09-29 用 `YZSysRun` 换成 SYSTEM 后，
同一台机器、同一个客户端立刻成功——`0x5` 其实是 **UIPI 的一致性检查**。现在本地把两种身份
并排复现（目标一律用 `YZSysRun` 起成 SYSTEM，放进路径含 `GZYZ` 的目录，配置固定为
`Flags=5 + AutoInject=0 + ExternalWindowFix=1`，只走跨进程纠正）：

* 断言 32：主程序是管理员（高完整性）时，写 SYSTEM 窗口**被拒且不静默**（记 WARN），
  目标仍是 `fullscreen=1`；
* 断言 33：主程序也换成 SYSTEM 后**成功**（状态由 `fullscreen=1` 变成 `fullscreen=0`，
  日志出现 `外部窗口纠正: 0x… -> x,y wxh`）。

**本轮实测的副产品（已补进本 README 的日志章节与 `工作顺序.txt`）**：阶段 B 第一次跑时断言红了，
但目标状态文件明明显示 `fullscreen=0`——功能是成功的，只是那几行日志不在我们读的文件里。
查证：**用 `YZSysRun` 以 SYSTEM 启动的主程序，日志写在 SYSTEM 的 `%TEMP%`**，即
`C:\Windows\Temp\YZTrainer\yzt.log`，而不是学生账户的 temp。这正是"必须用 YZSysRun 启动"
之后一直在用的路径，此前**没有任何文档写过**——现场手工捞 `yzt.log` 很容易捞到
"非 SYSTEM 那一次"。修法是阶段十四同时读两份日志（各自按本阶段新增的行匹配）。

实测：第一次 32 PASS/1 FAIL（红的正是日志通道），修好口径后 **33 PASS/0 FAIL**
（`sim-test-v0619.log`）。

### 2026-10-11（第九轮）v0.6.20：冻结开启时不会把主界面拍进去（33 → 35 项）

**问题**：防监视默认关闭，用户**手动**勾上的那一刻主界面正好开着；而冻结帧是目标进程里
`GetDC(NULL)` + `BitBlt` 抓的整屏 DIB，屏幕上有什么就冻什么——于是主界面被冻进教师
"监视/远程桌面"会看到的那张图，教师点开学生机一眼就能看见学生在用某个工具。

**先量清楚再动手**（一次性实验，开了个纯品红窗口，两条抓屏路径各采样同一像素）：

| 模式 | GDI `BitBlt(GetDC(NULL))` | DXGI 桌面复制 |
|---|---|---|
| `WDA_NONE`（对照） | `0xFF00FF` 被捕获 | `0xFF00FF` 被捕获 |
| `WDA_EXCLUDEFROMCAPTURE` | `0xCC898A` **被排除** | `0xA0A2B7` **被排除** |

对照点两次都是 `0x000000`（抓屏本身成功）。结论：本机两条路径**都**尊重这个亲和性，
而且被排除处露出的是窗口后面的桌面内容、**不是黑块**（这也是不用 `WDA_MONITOR` 的原因——
那会让窗口在捕获里变成一片黑，比窗口本身更可疑）。

**落地（两层 + 顺序）**：
* **主界面收进托盘**：不依赖任何抓屏路径，是硬保证；顺序必须是"**先隐藏 → 等桌面重绘
  （约 200ms）→ 再下发冻结位**"，因为钩子是收到配置才抓帧，顺序反了第一帧还是脏的。
  托盘气泡会说明"主界面已收入托盘，托盘图标或 Ctrl+Alt+F12 可再打开"。
* **`WDA_EXCLUDEFROMCAPTURE`**：作为将来 DXGI 路径的兜底，也覆盖"冻结期间用户又把界面唤出来"
  的情况；关闭冻结时复位为 `WDA_NONE`，并且只在"开启时可见、现在隐藏"时才恢复显示，
  不覆盖用户自己按 Ctrl+Alt+F12 拨过的状态。设置失败（Win10 2004 之前）只记 WARN 并明说
  只剩第一层，不假装成功。

**顺带记两个坑**（都写进脚本注释，免得后人重踩）：① `GetWindowDisplayAffinity` **跨进程读回来
总是 0**（设的时候成功、效果也实测存在），所以"设上了没有"只能由主程序**进程内读回**并写日志，
脚本里的跨进程枚举只作诊断；② `UiAppendLog` 只进界面日志、**不落文件**，断言要用 `YZLOG*` 的行。

实测：第一次 33 PASS/2 FAIL（两条都红在断言口径上），修好后 **35 PASS / 0 FAIL**
（`sim-test-v0620.log`）。

### 2026-10-11（第十轮）v0.6.21：DXGI 桌面复制"取证"钩子（35 → 37 项）

目标 ③ 仍卡在"监视走 DXGI 还是 GDI helper"，但比"走哪条"更危险的一个问题此前没人问：
**学生机上的 `DuplicateOutput` 会不会也被"接收广播"那条链用到？** 如果会，那么盲写 DXGI 冻结
就会把学生看教师广播的画面冻住——这种回归本机复现不了、现场也难排查。所以这一轮加了一条
**只计数、绝不改变行为**的取证钩子，把这个问题交给机房去回答。

* **默认关闭**：控制位 `YZ_CFG_TRACE_DUPLICATION` + ini 项 `TraceDesktopDuplication`（出厂 0）。
  关闭时本模块一行代码都不跑，也不会把 `dxgi.dll` 拖进目标进程。
* **开了也只计数**：只替换 `IDXGIOutput1` 真 vtable 的槽 22（`0x58 DuplicateOutput`），
  duplication 对象按对象换 vtable（拷贝表、只换槽 8/14）；所有调用**原样转发**，
  返回值与 out 参数一个字节都不改，不 sleep、不干预（与上一轮预演里"诚实实现"的结论一致）。
* 日志前缀 `[DXGI 取证]`：装钩子、建立桌面复制（前 3 次）、每 30 秒一条取帧统计
  （调用/超时/取到帧 + 折算帧率）。这些行会被镜像回主程序日志并进诊断包。
* 实现细节：`dxgi.dll` 与 `CreateDXGIFactory1` 全部**动态加载**，头文件只借类型声明即取
  `IDXGIOutput1` 实例读 vtable，**不调用** `DuplicateOutput`（那会占掉"每输出仅一个客户端"
  的名额，把厂商挤掉）。

本机实测（`sim-test-v0621.log`，阶段十六 = 新增 2 条断言）：

```
[DXGI 取证] 已装上桌面复制取证钩子（槽 0x58，只计数不改行为）
[DXGI 取证] 建立桌面复制成功（第 1 次，duplication=1105BE58）——这条链上的取帧会被计数，但行为不受影响
[DXGI 取证] 周期 取帧统计 #1: 调用=1 超时=0 取到帧=1（约 1000 帧/秒，累计 0 秒）
模拟端：dup: 完成 取到帧=20 超时=0（请求 20）
```

**机房怎么用**：ini 里设 `TraceDesktopDuplication=1` 再按【2】起主程序，然后照旧让老师点
一次监视/缩略图、放一次广播。日志里若只在**监视**时出现 `[DXGI 取证] 建立桌面复制成功`，
说明监视走 DXGI（冻结就钩那条链）；若在**接收广播**时也出现，就说明这条链不能盲冻——
那时得改用"只冻取帧、不冻建立"或退回 GDI/替换帧像素的路线。两种结论都能一眼看出来。

### 2026-10-11（第十一轮）v0.6.22：探针跨架构枚举进回归（37 → 39 项）

`工作顺序.txt`【1】让现场带的是 **x64 的 `YZProbe.exe`**，而学生端是 **32 位**进程。64 位进程枚举
WOW64 目标的模块必须带 `TH32CS_SNAPMODULE32`；漏掉它**不会报错、报告照样生成**，只是模块列表
为空——现场会得出"没有采集组件"的结论，**整趟数据作废**。探针四处 `CreateToolhelp32Snapshot`
都带了那个标志，但此前从没验过。

**本机验证（两个架构都跑一遍，报告逐字一致）**：把 32 位模拟目标放进路径含 `GZYZ` 的目录、
加载改名成 `ExdDtDup.dll` 的 32 位替身（`SysWOW64\version.dll`）后：

```
图形采集栈 PID=23580 YZSimTarget.exe：内存 ExdDtDup.dll=是 d3d11.dll=否 dxgi.dll=否 dwmapi.dll=否 gdi32.dll=是；
磁盘 KsFiles\ExdDtDup.dll=无；采集层=DXGI 桌面复制（ExdDtDup 已加载，冻结画面必须走 DXGI 路径）
采集组件扫描命中：PID=23580 YZSimTarget.exe → ExdDtDup.dll；
```

结论：**U 盘带 x64 探针没问题**，那份带法不用改。为了让这个结论以后不被悄悄改坏，
新增阶段十七两条断言（按进程枚举 + 全进程扫描两条口径各一条）。

顺带记第四个"红的是测试自己"的坑：阶段十七第一次跑 38 红 39 绿，原因是模拟目标直接从
`dist\Win32\` 起——而探针只对"远志路径形状"的进程输出图形采集栈行；全进程扫描不看路径，
所以只有那条红。规律与前三轮一致：**测试脚本里的环境形状必须与产品判据一一对应**
（路径是否远志形状、日志落在哪个文件、进程何时被杀）。

### 2026-10-11（第十二轮）v0.6.23：修掉探针一个会误导现场判读的保护级别 bug（39 → 41 项）

上一轮做阶段十七时，那份报告里我们**能正常注入**的模拟目标被标成 `PPL/WinTcbLight`——明显不对
（回归每一轮都在往那个进程里注入）。顺着查下去，是个会影响现场结论的真 bug：

**根因**：保护级别原来**只在 `p.isYuanzhi` 为真时**才查，而 v0.6.12 新增的"采集组件轻量扫描"
会在**之后**把进程晋级成远志——那类进程正是现场最想看的"真正持 `DuplicateOutput` 的那个"。
于是它的级别留在初值 `0`，而保护级别枚举里 `0 == PROTECTION_LEVEL_WINTCB_LIGHT` →
报告写成 `PPL/WinTcbLight`。现场读到这一列会认为"这个抓屏进程受 PPL 保护、动不了"，结论直接错，
而且错在最关键的进程上。本机 2/2 稳定复现（同一份 exe：放 `dist\Win32\` 报 PPL、放
路径含 `GZYZ` 的目录里就报"无"）。

**修法**：① 保护级别对每个进程都查，并复用已打开的 `QUERY_LIMITED` 句柄（`QueryProtectionLevelHandle`），
彻底避开"查询时机在晋级之前"；② `ProbeProcess` 增加 `protectionKnown`——查询失败就是**未知**，
绝不落进 `0 == WINTCB_LIGHT` 那一档；③ 报告与 JSON 都按它输出，未知时写
`未知（查询失败：句柄打不开，常见于 PPL/受保护进程）`，JSON 另给 `protectionKnown` 布尔。

**阶段十八（新增 2 项）**：把模拟目标放在**不带远志路径特征**的目录里、加载替身（于是只能靠
"采集组件扫描"晋级），断言它确实进了"远志相关进程"表，且"进程保护"列是"无"、不是 `PPL/WinTcbLight`。
修好后本机实测该行：`| 11408 | YZSimTarget.exe | ALCURT\34088 | 高(12288) | 是 | 无 | …`，
**41 PASS / 0 FAIL**。

**顺带修掉两处文档数字过期**（我自己的疏漏）：`工作顺序.txt`【1】里 `dist\Win32\YZTrainer.exe`
写的还是 483328（实际 **487936**）、备用 `dist\Win32\YZProbe.exe` 写的 308736（实际 **309760**）。
那段是现场核对"有没有拿到旧副本"的唯一凭据，**过期数字会被当成旧副本扔掉**。
教训：升版收尾时除了版本行，也要回头核对【1】里每一处字节数。

### 2026-10-11（第十三轮）v0.6.24：x64 探针 `--watch` 的跨架构也进回归（41 → 43 项）

现场采"教师监视时谁在抓屏"的主力通道是 **x64 探针的 `--watch`**，而此前验过的是"一次性扫描的
跨架构"（阶段十七）与"Win32 探针的 `--watch`"（阶段十）——**这两个的组合没人验过**。
`--watch` 走的是另一套代码（进程快照 + 采集组件扫描 + 触发判定 + 报告落盘），要是它对 32 位目标
漏了模块枚举，现场照样白跑一节课。

**本机实测**（32 位目标放 `GZYZ` 目录、加载改名成 `ExdDtDup.dll` 的替身；x64 探针先挂
`--watch --duration 22 --interval 2`，4 秒后目标起）：

```
触发落盘[window]：PID=2488 … 类=YZSimBroadcastWnd …（远志进程 + 覆盖整屏 + 无标题栏 + 置顶）
触发落盘[capture]：PID=2488 YZSimTarget.exe ExdDtDup.dll → …-watch-capture.json
watch 结束：共落盘 2 份报告
图形采集栈 PID=2488 YZSimTarget.exe：内存 ExdDtDup.dll=是 … 采集层=DXGI 桌面复制
```

触发、判词、退出三件事都正常。新增**阶段十九**两条断言把它钉住（报告内容 + 跨架构判词），
回归 **43 PASS / 0 FAIL**（`sim-test-v0624.log`）。

小插曲：第一次发起提权回归时 UAC 弹框被取消，脚本没跑起来；我没有反复弹框，而是停下来问了一句，
用户选择重试后才跑（与 AGENTS §1.2 一致）。

### 2026-10-11（第十四轮）v0.6.25：`InjectMethod=1` 进回归（43 → 45 项），并修掉回归自身的两个坑

README 第 121 行的现场指引写着"如果日志里只有 `VirtualAllocEx 失败`，就把 `InjectMethod` 改成 `1`
再试消息钩子"，而回归此前**只测过默认的 0（远程线程）与运行期自动回退**，从没测过"ini 里直接指定 1"。
这条路径要是坏了，现场照 README 操作会直接卡住。新增**阶段二十**三条断言：①日志出现
`已通过消息钩子注入 HHOOK=…`；②窗口化照常生效（说明引擎真被映射进目标）；
③不再尝试远程线程注入。实测 `消息钩子注入=True 已窗口化=True 未走远程线程=True`。

**这一轮同时修掉回归自身的两个坑**（都属"红的是测试、不是产品"）：

1. **日志轮转让断言瞎了**。产品在 `yzt.log` 超过 2MB 后改用 `yzt-1.log`（README 有写），而一次完整
   回归**必然越过这条线**（这次正好停在 2100029 字节）。原来的 `Get-LogLength`/`Read-LogTail`
   只读主日志，于是轮转之后的所有阶段读到的都是"空的新增日志"——阶段二十第一次跑就是这样误报的。
   修法：mark 从"字节数"改成"**各日志文件的长度快照**"（调用方接口不变），读取时对主日志与
   `<日志>-1.log` 各取自己新增的一段，并把"文件被重建/变短"当作从头读；改完做了单元自测。
2. **我自己往 PowerShell 里写了 C 风格注释 `/* … */`**（PowerShell 不认），脚本一启动就死、
   日志 0 字节——而 `[Parser]::ParseFile` 竟然报 0 个错误。教训：**别只信解析器的"通过"**，
   改完测试脚本至少要真跑一次看有没有输出。

顺带给现场补了一条：一次长课的日志很可能**同时有 `yzt.log` 与 `yzt-1.log` 两份**，捞日志两份都要拿
（已写进 `工作顺序.txt`【5】）。

### 2026-10-11（第十五轮）v0.6.26：界面/热键那条开关链进回归（45 → 48 项），顺带修两个真问题

`CmdSetFlag` 对四个功能位（窗口化/键鼠解锁/防监视/拦遥控）是**只发 `YZ_CMD_SET_*` opcode、
不发整份配置**——也就是说**用户勾复选框、按热键走的是钩子侧 `EngineSetFlag` 那条链**，
而回归此前只写 ini，从没碰过它。顺这条线查下去，抓到两个真问题：

1. **全局热键注册失败原来完全静默**。本机实测 `Ctrl+Alt+F9` 已被别的程序占用，
   `RegisterHotKey` 返回失败（`err=1409` 热键已注册），而代码不看返回值——用户按了没反应，
   只会以为程序坏了（现场很容易碰到：显卡驱动、输入法、录屏软件都爱占组合键）。
   现在五个热键逐个查返回值，失败记 WARN（含错误码与"改用复选框/托盘菜单"的提示）。
2. **`Ctrl+Alt+F12` 只"显示"不"隐藏"**（README 写的是"显示/隐藏"）。改成真正的切换。

**阶段二十一（新增 3 项）** 用**确定性驱动**测这条链：复选框走 `WM_COMMAND`+`BM_SETCHECK`
（与鼠标点击同一条代码路径），热键走直接投递 `WM_HOTKEY`（与系统投递同一个处理器）。
断言：勾上防监视 → ini 出现防监视位且抓帧哈希停住；取消 → 哈希又变化；`WM_HOTKEY(F12)`
→ 主窗口句柄归 0，再发一次恢复。实测 `复选框命中=True 勾选后冻结=True（ini Flags=13）取消后恢复实时=True`、
`WM_HOTKEY(F12): 初始句柄=4786214 隐藏后=0 再发后=4786214`。

为什么不用合成按键：先按 `keybd_event` 写，F9 那次败于"热键根本没注册上"；换成注册成功的
F11/F12 后**仍然**没反应。两个隔离实验说明了原因——① 极小程序自己注册 `Ctrl+Alt+F11` 再用
合成键触发，**两种方式都能收到 WM_HOTKEY**（注入方式没问题）；② 只起主程序、不起模拟目标时
注入 F11，ini 的 Flags 从 5 变 13（热键与 opcode 链本身完全正常）。所以失败只出现在
"合成按键 + 模拟目标在场"这个组合上（测试环境的输入注入问题），不值得让回归依赖它。

### 2026-10-11（第十六轮）v0.6.27：托盘菜单那条路径进回归（48 → 50 项）

`v0.6.20` 起"冻结会把主界面收进托盘"——也就是说**冻结期间托盘菜单是唯一的图形开关入口**
（用户看不到复选框，也未必记得住热键）。而托盘菜单走的是 `TrackPopupMenu` 之后由 `WM_COMMAND`
（id 3001..3007）分发到 `TrayMenuCommand` 的那条路，此前**一条断言都没有**；好在它可以
用 `SendMessage(WM_COMMAND, id)` 确定性驱动，不必碰托盘图标。

**阶段二十二（新增 2 项）**：
* 断言 50：托盘"防监视（冻结画面）"能打开冻结（`ini` 出现防监视位、抓帧哈希停住）；
* 断言 51：冻结自动收起主界面后，托盘"显示主界面"能把它恢复，而且**冻结不能被解开**
  （哈希仍停住）——后半句是重点：现场最常见的动作就是"冻结着、但我要把界面调出来"，
  若为此解掉冻结，教师那边立刻看到实时画面。

实测 `托盘防监视生效=True（ini Flags=13） 冻结后主界面被收起=True 托盘恢复界面且仍冻结=True（句柄=37422702）`，
回归 **50 PASS / 0 FAIL**（`sim-test-v0627.log`，一次通过）。

**顺带盘点了"能改功能位的入口"**：① ini（各阶段基线）；② 界面复选框（阶段二十一）；
③ 热键处理器（阶段二十一，另有两个隔离实验证明 `RegisterHotKey` 与合成按键都正常）；
④ 托盘菜单（本阶段）——四条入口现在各有断言。

### 2026-10-11（第十七轮）v0.6.28：两个 ini 边界项进回归（50 → 53 项）

按上一轮的盘点收掉两个"现场可能真去改"的配置项：

* **`WindowPercent` 的 20/100 钳制**（产品在 `config.cpp` 与 `hooks_window.cpp` 各夹一次）：
  填 5 不该得到几乎看不见的窗口、填 200 应当等于 100；
* **`AutoExportOn*` 全关**：关掉就得真关掉，阶段期间不得新增任何 `diag-*-auto` 目录。

实测 `WindowPercent=5 → 窗口/显示器宽=0.2 ；=200 → 1（样式=0x14CF0000 有标题栏=True） 自动导出关闭时新增诊断包=0`，
回归 **53 PASS / 0 FAIL**（`sim-test-v0628.log`）。

**这一轮的价值几乎全在"把测试判据写对"**（产品一行没动）：阶段二十三跑了四次才绿，四次红都是判据问题——
① 用两次 `-match` 取值，第二次把 `$Matches` 覆盖了，rect 分组全成 0；② 把 `fullscreen=0` 当条件，
而模拟目标的 `fullscreen` **只看几何**（100% 的窗口带标题栏也会被记成 1）；③ 去掉那个条件后，
5% 那轮一上来就命中了**启动时的无边框全屏**、没等窗口化，改成只采"带标题栏"的状态才行。
另外 UAC 这一轮被取消两次，我没有反复弹框，都停下来与你确认。

### 2026-10-11（第十八轮）v0.6.29：诊断包"一包搞定"——各来源日志一起进包（53 → 55 项）

这一轮由你（用户）的一个提问引出：*"你说每个目录的 yzt、yzt-1.log 都要，那有没有设置退出时一并复制到
主程序运行目录，免得匆忙时遗漏？"* 查下来——**一半已经有了，但有三个真缺口**。

已有的：诊断包本来就写在 `YZTrainer.exe` 同目录，包里含 `yzt.log`/`yzt-1.log`/`yzt-2.log`/
`remote-exec*.log`/`YZProbe-report-*`/`snapshot.txt`，退出时也会自动导一份（默认开）。

补掉的三个缺口：
1. **只收自己那一个日志目录**：SYSTEM 身份跑时日志在 `C:\Windows\Temp\YZTrainer`、普通身份跑时在
   用户 temp，两处都跑过时任意一个包里只有一半证据——这正是我前几轮反复叮嘱"两份 yzt.log 都要拿"
   的原因，也正是匆忙时最容易漏的；
2. **只认 `-1/-2` 两个固定名字**：一次长课 2MB 一份，很容易轮转到 `-3` 就漏了；
3. **exe 目录不可写时直接放弃**（只记一条 WARN），日志就只剩 temp 里那份。

改法：按 `yzt*.log` / `remote-exec*.log` **通配枚举**收文件；三处来源一起收（本进程原名、
`C:\Windows\Temp\YZTrainer` 带 `-sys` 后缀、其它用户 profile 带 `-<用户名>` 后缀）；
`snapshot.txt` 新增 `== 日志来源 ==` 把来源列清楚；exe 目录不可写时**回落到日志目录**再导一次。

本机实测一份自动导出的包：
`yzt.log`(2100029) + `yzt-1.log`(984385) + **`yzt-sys.log`(37963)** + `remote-exec.log` + `snapshot.txt`，
快照里写着 `own=C:\Users\34088\AppData\Local\Temp\YZTrainer; sys=C:\Windows\Temp\YZTrainer`。
新增阶段二十四两条断言把它钉住，回归 **55 PASS / 0 FAIL**（`sim-test-v0629.log`）。
`工作顺序.txt`【5】里那条"两份 yzt.log 都要拿"也已改写成"**优先整包带走**，包内已含各来源日志"。

### 2026-10-11（第十九轮）v0.6.30：诊断包再补齐（探针产物 + 打包清单）＋回归开场自检

**由用户两个问题引出**：① 56 项断言跑一轮太慢；② 其它要带回来的数据是不是也像日志一样退出时自动收拢。

* **诊断包再补两样**：探针产物从"只认 `YZProbe-report-*`"放宽到 **`YZProbe-*.*`**——`--watch` 落下的
  `YZProbe-watch-<时间戳>.log` 以前收不进来，而它正是现场判"没采到"还是"没在跑"的证据；
  快照新增 **`== 打包清单 ==`**（每个文件名 + 字节数 + 共几项），现场一眼就能确认该带的都带了。
  新增断言 57 把它钉住。
* **回归提速**：先做了一版"条件轮询"（把主程序起来后的固定长等换成轮询模拟目标状态），
  实测总用时从（未计时的）约十分钟降到 **338 秒**，但**冒出 15 项假红**——轮询被上一阶段残留的
  窗口状态提前满足，阶段在工具动手前就往下走了。已回退：现在只保留**安全**的那部分
  （两个探针 `--watch` 的 `--duration` 26/22 → 14），实测 **383.6 秒 / 56 项全 PASS**。
  正确的轮询做法（以启动主程序那一刻的状态文件时间戳为基线，且要求状态确实变了）留待下一轮。
* **顺手加了开场自检**：主程序用全局互斥体防重入，**只要有一个残留实例在跑**，本脚本启动的每一份
  都会静默退出、整轮全变假红（我确实这么白等过一轮）。现在开场先查 `YZTrainer` 进程，发现就直接
  报清楚 pid 并中止；对残留的 `YZSimTarget` 打一条警告。

### 2026-10-11（第二十轮）v0.6.31：托盘图标改用固定 GUID 身份（不再堆死图标）

起因是你看到的那一墙托盘图标。根因：托盘图标原来用 `uID = 1` 的**按进程**身份，而主程序被**强杀或崩溃**时
`Shell_NotifyIcon(NIM_DELETE)` 永远不会执行，Explorer 就把这个图标当"死图标"留着——我的回归每阶段都用
`Stop-Process`，几十轮下来就在 4K 175% 缩放的任务栏上堆出几十个同样的图标（还有一次残留实例雪上加霜）。

改法（`ui.cpp`）：`NIF_GUID` + 一个固定 GUID，**两级尝试**——
① 先用固定 GUID 注册，成功就记一条 INFO（`托盘图标: 以固定 GUID 注册成功…`）；
② 若老 shell 不认 `NIF_GUID`，回落到原来的 `uID` 方式并记 WARN（明说"被强杀时可能留死图标"）。
气球提示的 `uFlags` 也补上了 `NIF_GUID`（否则按 uID 找不到图标、提示会丢）。

> **2026-10-13 实测更正（多轮）**：`NIM_ADD(NIF_GUID)` 的成败在本机**随上下文而变**——免提权跑（`build.ps1 -NoElevate` 的 asInvoker 构建）连续 3 次都返回 `E_FAIL(0x80004005)` 并回落 uID；而**提权后**的交付构建跑回归时**注册成功**（同一份源码、同一台机器、相隔几分钟）。
> 已排除：清残留槽位后重试（已加该动作，仍失败）、`uVersion` 在 add 前/后声明、`cbSize`
> （Win32 构建下 956 就是 v4 尺寸，x64 才是 976）。⇒ **两条路都会走到、且都有日志**，暂不要对外声称"GUID 一定生效"或"一定失败"；
> 固定 GUID 复用同一槽位这条**在本机不成立**；被强杀仍会留死图标（悬停托盘或重启 Explorer 可清）。
> 真正消除死图标的是 **v0.6.34 起回归改用优雅退出**（会走 NIM_DELETE）。

本机实测（Windows 10 IoT LTSC 21H2）：
* 干净 shell 下：`托盘图标: 以固定 GUID 注册成功（被强杀/重启后复用同一槽位，不会在任务栏堆死图标）`；
* 回归环境（每阶段强杀）下：`托盘图标: GUID 方式失败（err=0），已回落 uID 方式——被强杀时任务栏可能留下死图标`
  ——Explorer 仍留着上一次被杀实例的 GUID 条目，于是同一 GUID 再注册会失败。这恰好印证了"死图标"的根因：
  **被杀死的实例永远不会调 `NIM_DELETE`**。彻底解决要靠"优雅退出"（见目标 ④），GUID 只是"新实例复用槽位"这一层。
* 两种尝试都失败时也会记 WARN（v0.6.31 补的：静默失败最坏，用户只会看到"图标没了"，日志里毫无线索）。

新增断言 58 钉住的是"**注册结果必须有明确记录**"（GUID 成功 / 回落 uID / 两次都失败三条路都要留痕），
而不是"必须走 GUID"——后者在任务栏堆满死图标的环境下本来就做不到。已经堆出来的死图标，靠鼠标扫过托盘区
（Explorer 会在悬停时清理）或重启 Explorer 清掉。

### 2026-10-12（第二十一轮）v0.6.32：抓屏子进程识别 + 按 PID 注入（机房数据驱动）

**机房数据（`archive\YZ-26.10.12\`）把冻结画面的路线一次性定死了**：教师"监视/远程桌面"时，
**学生端自己**拉起 `C:\Windows\SysWOW64\rundll32.exe "<...>\KsFiles\ExdDtDup.dll",RunCapture 0`
（审计日志 5 次会话 5 个新进程，`origin=remote`，结束由 `tskill` 收尾），**那条 DXGI 桌面复制就在子进程里**——
做在 `Yistart.exe` 里的冻结自然没用（这正是机房"开了防监视教师端仍看到实时画面"的原因）。

本版做的"看得见的一半"：**双触发识别 → 按 PID 注入 → 权限判词**

* `hooks_exec.cpp`：`CreateProcessW/A` 原函数返回后看命令行，命中 `ExdDtDup.dll`/`ExdElite`/`ExdPaint`/
  `ScreenRecoder.exe` 就在审计结果串尾打 `[抓屏子进程 pid=N]`（只观察、不改写调用）。机房里这条**确实会镜像
  回宿主**（归档日志有 `[远程执行审计] … RunCapture 0` 行），所以第一条触发在机房可用。
* `capturewatch` 的采集栈监控命中**也**触发注入（身份无关）——机房实测这条 4 次全中，本机也跑通了。
* 新 ini 键 **`InjectCaptureChild`**（默认 1 = 识别并按 PID 注入；0 = 只识别不干预）。
* 注入结果一定写日志：`抓屏子进程 pid=N：已注入（冻结画面将在这个进程里做）`，失败则给原因——
  现场要判的第一件事就是"能不能进这个进程"（客户端上 `PROCESS_VM_OPERATION` 被内核剥夺，子进程尚未验证）。

**免提权测试构建**（`pwsh -File build.ps1 -NoElevate`）：产出 `build\noelev\YZTrainer.exe`（asInvoker，
只写 `build\`，**绝不碰 `dist` 交付目录**）。主程序侧的验证从此不必等 UAC——本版整条链就是用它在
本机跑通的（`build\logs\verify-capture-child-inject-v0632.log`：采集栈监控命中 → 抓屏子进程出现 →
`pid=18844：已注入`）。

新增：`src\YZTrainer\capturechild.h`（标记解析，纯函数）+ `tests\unit\capturechild_parse_test.cpp`
（12 个用例，含机房真实审计行，**不需要管理员**即可跑）；回归新增阶段二十五 2 条断言（共 25 阶段 59 项）。

### 2026-10-13（第二十三轮）v0.6.36：抓屏子进程的"补充状态通道"（修掉一个现场会致命的缺陷）

**发现的缺陷**（本机复现）：管道服务端**同时只服务一个客户端**。现场顺序是"学生端先被注入并占住管道"→
我们随后注入**抓屏子进程**（`rundll32 + KsFiles\ExdDtDup.dll`）→ 它**永远连不上管道、拿不到配置帧**
⇒ v0.6.33 的冻结（靠配置帧里的防监视位武装）**在机房不会被武装**。
复现日志：`sim#1 → 已连接到主程序管道`；`sim#2 → 已注入目标进程 pid=…（第 1…12 次，每 2 秒一次）`
而**始终没有**"已连接到主程序管道"。顺带暴露第二个问题：看门狗会对这个永不回报的 pid 每 2 秒重复注入。

**修法**（`v0.6.36`）：给冻结加一条**不依赖管道**的补充通道——
主程序把当前意图写进 `C:\ProgramData\YZTrainer\capture-state.txt`（`AntiMonitor=0/1`；SYSTEM 与用户身份
都读得到），启动时与每次功能位变化时各写一次；抓屏进程里的钩子**每约 2 秒读一次**，
条件满足（本进程确实加载了采集组件 + 文件说防监视开着）就 `DupTraceInstall(true)` + `DupTraceSetFreeze(true)`；
配置帧若到达仍以配置帧为准。读不到/首次不存在只记一条 INFO（`err=2` 属正常），不再吓人。

**本机实测（免提权，两个目标进程模拟现场）**：

```
21:26:50  抓屏子进程出现: pid=5572 → 补充状态通道: 已写 …（AntiMonitor=1）
21:26:50  抓屏子进程 pid=5572：已注入
21:26:52  补充状态通道: 读到 AntiMonitor=1（本进程是抓屏进程=1，当前已冻结=0）
21:26:52  DXGI 冻结: 已开启（该进程内的桌面复制将取不到新帧）
```

即"客户端占着管道、抓屏子进程连不上"这个现场条件下，冻结**依然被武装** ✔。

**顺带记一条会坑人的构建陷阱**：`build.ps1 -NoElevate` 增量构建时 `YZTrainer.exe` 可能早于
`YZHook.dll` 完成，于是内嵌的是**上一版**钩子载荷——改了 `src\YZHook\` 之后必须再跑一次该构建，
并用字符串搜索确认 exe 里确实是新文案（本轮我就因此先测到了旧钩子）。

### 2026-10-13（第二十四轮）v0.6.37：同一 pid 重复注入的指数退避

管道服务端同时只服务一个客户端，所以"第二个被注入的进程"（现场的抓屏子进程）永远连不上、
状态永不回报，而旧逻辑的固定 2 秒节流会对它**无限重注入**（§39.7 实测被注入 12 次以上）。
现在按注入次数退避：前 3 次仍 2 秒，之后 8 / 16 / 32 / 60 秒封顶，并在首次退避时打一条 INFO
（`pid=… 已连续注入 3 次仍未见回报状态（例如它连不上管道），重试间隔放宽到 8 秒并继续退避`）。

本机实测（两个目标进程，免提权）：`第 2/3/4 次`间隔 2 秒 → 退避 INFO → `第 6 次`间隔 8 秒 → `第 7 次`间隔 16 秒。

### 2026-10-14（第二十五轮）v0.6.38：管道服务端已支持多客户端（正式修法）

v0.6.36 只是绕过（`capture-state.txt` 补充通道），根因在管道：**原来同一时刻只服务一个连接**
（监听线程自己既接入又服务）。现在改成：监听线程只负责 `CreateNamedPipe` + `ConnectNamedPipe`，
接到一个就交给**独立服务线程**，自己立刻回去接下一个；`IpcSend` 变成**广播**给所有连上的客户端。

两个多客户端特有的坑（都已处理）：
1. **overlapped 事件不能共用**——每个连接必须有各自的事件，否则多个服务线程会被同一个事件唤醒、
   拿到彼此的结果；`g_ioEvent` 现在只留给监听线程，服务线程用各自槽位的 `ioEvent`。
2. **停止时要用 `CancelIoEx`** 唤醒阻塞在 `ReadFile` 上的服务线程（`CancelIo` 只取消调用线程自己的操作），
   再等线程退出，避免退出流程挂住。

另外把注入判定从"主客户端是不是这个 pid"改成"**这个 pid 是否已有连上的客户端**"
（`IpcIsClientPidConnected`）——多客户端之后，只看主客户端会让抓屏子进程被反复重注入。

**本机实测**（两个目标进程，免提权）：`Hook DLL 已连接: pid=<客户端>` 与 `pid=<抓屏子进程角色>`
**两条都出现**，且第二个进程收到 `应用配置: flags=0x0003000D` 并在 3 秒后 `DXGI 冻结: 已开启`
（原来是永远连不上、只能靠补充通道）。回归新增断言 62 守住这一点（61 → 62 项）。

### 2026-10-14（第二十六轮）v0.6.39：多客户端下的考试熔断改为"取或"粘滞

**v0.6.38 自己引入的风险**：管道支持多客户端后，抓屏子进程也会上报 `YZ_EVT_STATUS`，
而旧代码是直接赋值 `g_app.examMode = (status.examMode != 0)` —— 一旦某个**不检测考试**的进程
（例如 rundll32 抓屏子进程）在考试期间上报 0，就会把考试熔断**清掉、功能重新打开**，属硬边界问题。
（单客户端时代不存在：那个进程根本连不上。）

现在：考试状态**按客户端分别记录**，`g_app.examMode = 所有连上客户端的取或`；
客户端断开时清掉它自己那条记录并重算。任一客户端报"考试中"就保持熔断，全部报 0 才解除。
另外 `g_app.status`（`hooksInstalled` 等）**只接受主客户端**（连接顺序第一个）的状态帧。
方向性说明：取或只会让熔断**更难被误解除**（更保守）；残余代价是"某个一直报考试中的进程会让
功能保持停用"，而其生命周期受该进程自身限制（抓屏子进程随教师端监视结束而退出）。

### 2026-10-14（第二十七轮）v0.6.40：采集栈监控扫描周期可配（回归提速的大头）

目标 ④ 的 ~300 秒硬目标之前一直差一截，原因是**没把时间花在哪量出来**。加了逐阶段计时后一眼看到
大头：采集栈监控的常规扫描周期是**硬编码 30 秒**（`kScanIntervalMs`），而阶段七（采集栈监控）与
阶段二十五（抓屏子进程识别）都在**等它命中** ⇒ 每阶段白等一次扫描。

现在改成可配 `CaptureScanIntervalMs`（**默认仍是 30000，现场行为不变**；下限 500），
回归里那两个阶段设成 3000。实测 A/B（本机，免提权）：

```
默认 30000：从起子进程到"出现采集组件"命中 26.9 秒
设成 3000 ：同一步 5.7 秒          ⇒ 每阶段省 21.2 秒
```

另加**逐阶段计时输出**（`[计时] 阶段X 用时 N 秒`）：以后优化都先看画像，不凭感觉缩短等待。

### 现场工具（tools\，v0.6.40 起）

机房那一趟只有一次机会，工具都按"能在 Windows PowerShell 5.1 上跑、错误流为 0"标准验过（脚本存为 UTF-8 带 BOM）。

| 工具 | 用途 |
|---|---|
| `tools\field-ini\YZTrainer.ini` | 现场配置模板：`Flags=45`（窗口化+键鼠解锁+防监视+假全屏）+ `TraceDesktopDuplication=1`（冻结取证）+ 采集时导诊断包、退出不导。拷到 exe 同目录改名 `YZTrainer.ini` 即可。**注意：该文件必须无 BOM**——带 BOM 时 `GetPrivateProfileInt` 会把首行当成 `\xEF\xBB\xBF[General]`，`[General]` 段不存在，整份配置静默回落到默认值。 |
| `tools\collect-field-data.ps1` | 一键回收：扫 `%TEMP%\YZTrainer`、`C:\Windows\Temp\YZTrainer`（主程序以 SYSTEM 跑过时的落点）、`C:\ProgramData\YZTrainer` 与当前目录，收**日志含全部轮转档** + `diag-*` 诊断包 + 探针产物 + ini，写 `manifest.txt`（含主日志 SHA256 前 16 位）并打 zip。 |
| `tools\input-probe.ps1` | 键鼠被屏蔽时量"哪一层被拦"：A `SendInput`、B `PostMessage`、C 自装 `WH_KEYBOARD_LL`（**合成/物理事件分开计数** + 自校准 + 要求人工确认"确实按了物理键"）、D **只读**驱动栈侦察（键盘/鼠标/HID 类的 Upper/LowerFilters 与运行中的相关内核驱动）。本工具自身不安装任何驱动。 |
| `tools\preflight-check.ps1` | 出发前一把过的检查：交付产物版本与出厂 ini、**内嵌钩子载荷是否为当前源码**（v0.6.35 踩过"上一版载荷"）、三个现场工具在 pwsh7 与 5.1 下是否都能解析、现场 ini 是否无 BOM、git 是否干净、有无残留进程。退出码 0=全绿 / 1=有 BAD 项 |
| `tools\analyze-field-data.ps1` | 回来后的判读：对回收包一条命令出表——客户端连接数与 pid、抓屏子进程识别/注入、冻结是否被武装、补充通道读写、`[DXGI 取证] 取帧统计`、考试信号、UIPI 失败数、WRN/ERR 前 10 汇总、以及键鼠探针 A/B/C/D 的结论 |
| `tests\timeguard.ps1` | 运行时间红线守门（中午 13:55 / 晚间 22:45）：打印剩余时间与档位（`OK` / `SHORT-ONLY` / `STOP`）；`-Watch` 可在到点后清理残留进程。 |

用法（Windows PowerShell 5.1 亦可）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\collect-field-data.ps1 -OutDir D:\带回
powershell -NoProfile -ExecutionPolicy Bypass -File tools\input-probe.ps1
pwsh -NoProfile -File tests\timeguard.ps1
```

判读要点（完整口径见 `工作顺序.txt`）：冻结生效的证据链是
`抓屏子进程出现: pid=…` → `应用配置: flags=…`（或 `补充状态通道: 读到 AntiMonitor=1`）→ `DXGI 冻结: 已开启`；
量化统计看钩子的 `[DXGI 取证] 取帧统计 #N: 调用=… 超时=… 取到帧=…`（冻住时"取到帧"不再增长）。
## 免责声明与使用边界

### 2026-10-13（第二十四轮）v0.6.37：同一 pid 重复注入的指数退避

管道服务端同时只服务一个客户端，所以"第二个被注入的进程"（现场的抓屏子进程）永远连不上、
状态永不回报，而旧逻辑的固定 2 秒节流会对它**无限重注入**（§39.7 实测被注入 12 次以上）。
现在按注入次数退避：前 3 次仍 2 秒，之后 8 / 16 / 32 / 60 秒封顶，并在首次退避时打一条 INFO
（`pid=… 已连续注入 3 次仍未见回报状态（例如它连不上管道），重试间隔放宽到 8 秒并继续退避`）。

本机实测（两个目标进程，免提权）：`第 2/3/4 次`间隔 2 秒 → 退避 INFO → `第 6 次`间隔 8 秒 → `第 7 次`间隔 16 秒。

### 2026-10-14（第二十五轮）v0.6.38：管道服务端已支持多客户端（正式修法）

v0.6.36 只是绕过（`capture-state.txt` 补充通道），根因在管道：**原来同一时刻只服务一个连接**
（监听线程自己既接入又服务）。现在改成：监听线程只负责 `CreateNamedPipe` + `ConnectNamedPipe`，
接到一个就交给**独立服务线程**，自己立刻回去接下一个；`IpcSend` 变成**广播**给所有连上的客户端。

两个多客户端特有的坑（都已处理）：
1. **overlapped 事件不能共用**——每个连接必须有各自的事件，否则多个服务线程会被同一个事件唤醒、
   拿到彼此的结果；`g_ioEvent` 现在只留给监听线程，服务线程用各自槽位的 `ioEvent`。
2. **停止时要用 `CancelIoEx`** 唤醒阻塞在 `ReadFile` 上的服务线程（`CancelIo` 只取消调用线程自己的操作），
   再等线程退出，避免退出流程挂住。

另外把注入判定从"主客户端是不是这个 pid"改成"**这个 pid 是否已有连上的客户端**"
（`IpcIsClientPidConnected`）——多客户端之后，只看主客户端会让抓屏子进程被反复重注入。

**本机实测**（两个目标进程，免提权）：`Hook DLL 已连接: pid=<客户端>` 与 `pid=<抓屏子进程角色>`
**两条都出现**，且第二个进程收到 `应用配置: flags=0x0003000D` 并在 3 秒后 `DXGI 冻结: 已开启`
（原来是永远连不上、只能靠补充通道）。回归新增断言 62 守住这一点（61 → 62 项）。

### 2026-10-14（第二十六轮）v0.6.39：多客户端下的考试熔断改为"取或"粘滞

**v0.6.38 自己引入的风险**：管道支持多客户端后，抓屏子进程也会上报 `YZ_EVT_STATUS`，
而旧代码是直接赋值 `g_app.examMode = (status.examMode != 0)` —— 一旦某个**不检测考试**的进程
（例如 rundll32 抓屏子进程）在考试期间上报 0，就会把考试熔断**清掉、功能重新打开**，属硬边界问题。
（单客户端时代不存在：那个进程根本连不上。）

现在：考试状态**按客户端分别记录**，`g_app.examMode = 所有连上客户端的取或`；
客户端断开时清掉它自己那条记录并重算。任一客户端报"考试中"就保持熔断，全部报 0 才解除。
另外 `g_app.status`（`hooksInstalled` 等）**只接受主客户端**（连接顺序第一个）的状态帧。
方向性说明：取或只会让熔断**更难被误解除**（更保守）；残余代价是"某个一直报考试中的进程会让
功能保持停用"，而其生命周期受该进程自身限制（抓屏子进程随教师端监视结束而退出）。

### 2026-10-14（第二十七轮）v0.6.40：采集栈监控扫描周期可配（回归提速的大头）

目标 ④ 的 ~300 秒硬目标之前一直差一截，原因是**没把时间花在哪量出来**。加了逐阶段计时后一眼看到
大头：采集栈监控的常规扫描周期是**硬编码 30 秒**（`kScanIntervalMs`），而阶段七（采集栈监控）与
阶段二十五（抓屏子进程识别）都在**等它命中** ⇒ 每阶段白等一次扫描。

现在改成可配 `CaptureScanIntervalMs`（**默认仍是 30000，现场行为不变**；下限 500），
回归里那两个阶段设成 3000。实测 A/B（本机，免提权）：

```
默认 30000：从起子进程到"出现采集组件"命中 26.9 秒
设成 3000 ：同一步 5.7 秒          ⇒ 每阶段省 21.2 秒
```

另加**逐阶段计时输出**（`[计时] 阶段X 用时 N 秒`）：以后优化都先看画像，不凭感觉缩短等待。



本项目仅供**教育研究**与**技术学习**目的使用，为学习、研究相关软件在 Windows 用户态的运行机制（进程注入、API Hook、窗口与输入子系统）而编写，**禁止用于任何非法用途**。作者的出发点与 JiYuTrainer 相同：在**不影响正常教学**的前提下改善个人课堂体验。

* **与厂商无关联**：本项目由个人独立开发，与广州远志公司及其关联方无任何关系，未获其授权、认可或赞助；文中出现的产品名与商标归各自权利人所有，此处仅作指称性说明。
* **使用者自负其责**：你必须自行确保使用行为符合所在地法律法规、所在学校规章制度以及相关软件许可协议。因使用本软件导致的系统故障、数据丢失、账号或设备被封禁、纪律处分及其他任何后果，概由使用者自行承担，作者不承担任何责任。
* **禁止的使用场景**：严禁用于考试、测验、监考等任何评估环节；严禁破坏教学秩序、侵犯他人隐私、干扰他人设备；严禁任何攻击、牟利或二次分发牟利行为。规避技术措施与破坏计算机信息系统的行为，可能触犯《著作权法》相关条款及《刑法》第 285、286 条。
* **不规避技术措施**：本项目不以破解、复制、传播他人软件为目的，仓库内不包含任何厂商文件；请自行通过合法渠道获取相关软件。
* **无担保与已知风险**：软件按“现状”提供，不附带任何明示或默示担保。它需要管理员权限并向其他进程注入代码，可能被防病毒软件、EDR 或学校终端管理系统拦截、隔离或告警。
* **禁止恶意用途**：禁止将本项目或其衍生代码用于免杀、反检测、捆绑传播或任何攻击性用途。
* **考试模式硬边界**：程序检测到考试/测验的**强信号**时会立即自动停用全部功能；默认开启的"考试模式守护"（`EnableExamGuard`）只做判定，**关闭它等于停止检测，不改变本项目的使用边界**——任何考试、测验、监考场景都禁止使用本工具。

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

### 2026-10-12（第二十二轮）v0.6.33：冻结画面（DXGI 桌面复制）实现并验证

机房数据把路线定死了：教师"监视/远程桌面"时**学生端自己**拉起
`rundll32.exe "<...>\KsFiles\ExdDtDup.dll",RunCapture 0`，那条 **DXGI 桌面复制就在这个子进程里**——
做在 `Yistart.exe` 里的冻结没用（这正是机房"开了防监视教师端仍看到实时画面"的原因）。
v0.6.32 解决"识别 + 按 PID 注入"，本版补最后一步：**在那颗进程里把桌面复制冻住**。

* `hooks_duptrace.cpp`：冻结期间 `AcquireNextFrame` 桩**不转发**，按调用方 timeout **诚实等待**
  （`min(timeout,100)` 毫秒；timeout==0 给 8 毫秒）后返回 `DXGI_ERROR_WAIT_TIMEOUT`；不伪造帧、不排空
  ——调用方拿不到新帧，画面停在最后一帧（§27 本机排练：忙等会打到 3.54M 次/秒，诚实等待 ~32 次/秒）。
* `engine.cpp`：`ProcessLoadedCaptureComponent()` 查**本进程自己加载的模块**里有没有 ExdDtDup/ExdElite/
  ExdPaint/ScreenRecoder；冻结条件 = **本进程是抓屏进程 且 防监视开着**。自检而不是靠宿主下标记，
  是因为同一份配置广播给所有 Hook 实例，只有"你加载了采集组件"能把它们分开 ⇒ 客户端不会被误冻，
  "接收广播"结构性安全。
* 冻结开关与判定**镜像回宿主日志**：现场看 `DXGI 冻结: 已开启/已关闭` 与
  `[DXGI 判定] captureProc=… wantFreeze=… flags=0x…` 即可判断"为什么没冻上"。

**本机实测（免提权，`build\logs\verify-freeze-dxgi-v0633.log`）**：

```
【对照】Flags=1  → captureProc=1 wantFreeze=0 flags=0x00030001 → 取到帧=60 超时=0（请求 60）
【冻结】Flags=45 → captureProc=1 wantFreeze=1 flags=0x0003002D → 取到帧=0  超时=60（请求 60）
```

（`Flags=45` 即机房现场那套；`YZ_FLAG_ANTI_MONITOR=0x8`。替身必须叫 `YZSimTarget.exe`——钩子的宿主
身份自检只认远志学生端/模拟目标名，改名会让整个 Hook 静默不启用。）
