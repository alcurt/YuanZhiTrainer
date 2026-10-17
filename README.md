# YZTrainer

> **v0.6.48** · 一款**远志多媒体教学管理软件 V9.0 网管版学生端**的解控软件 · 仅供**教育研究**与**技术学习**，禁止任何非法用途 · 与厂商无关联，使用者自行承担全部后果。

> **部署硬要求（2026-09-29 机房实测）**：学生端以 SYSTEM 运行，而本程序默认是管理员（高完整性）。
> 两者差一级完整性，`VirtualAllocEx` 注入、`SetWindowsHookEx` 消息钩子、跨进程改窗口**三样都会被 UIPI 挡住**
> （已于 09-23、09-29 两次实测）。**必须用 `YZSysRun.exe -- YZTrainer.exe` 以 SYSTEM 身份启动主程序**：
> 同一台机器、同一个客户端，换成 SYSTEM 后消息钩子立刻落进 `Yistart.exe`（29 个 hook、管道连通）、
> 跨进程改窗口也不再报 0x5。v0.6.5 起程序在连续注入失败时会打印这条提示，并运行期自动改用消息钩子注入。

针对 **广州远志 YZinfo 多媒体教学网络系统 V9.0 网管版学生端** 的课堂辅助工具（普通学生端同样适用，实测机房部署的是网管版），参照 JiYuTrainer 的思路重新实现（不复制其代码）。

## 它做什么

* **广播窗口化**：教师端进行全屏广播时，把全屏、置顶、无边框的广播窗口改成可自由拖动缩放的普通窗口，画面与声音继续，你可以边看老师演示边操作自己的电脑。**注入进不去也能用**：进程内 Hook 拿不到目标进程时，改由主程序跨进程改窗口样式（`ExternalWindowFix`，默认开）。
* **假全屏（默认关闭）**：广播窗口继续铺满整屏、外观不变，只把它从最上层拿下来——教师端看到的仍是"全屏广播"，而你依次按 Alt+Tab 切过去的窗口能盖在广播上面，配合键鼠解锁就能操作自己的电脑。它与窗口化互斥且**优先于窗口化**。注意它只改窗口层级，**不隐藏内容**：教师端若在抓屏，看到的还是你的真实屏幕，要遮内容得靠防监视（冻结帧）。
* **教师端远程执行审计（始终开启）**：记录学生端进程里出现的 `CreateProcessA/W`、`WinExec`、`ShellExecuteExW`、`ExitWindowsEx` 调用（教师端的"远程执行命令/关机/重启/注销"最终都落在这里），**只写日志、不拦截、不改行为**。审计文件为日志目录下的 `remote-exec.log`（超过 2MB 顺延 `remote-exec-1.log`，不删除旧文件），并把"疑似教师端下发"的条目同时写进界面日志。考试模式下连审计也停。
* **解除键鼠锁定（2026-10-16 机房实测改判：用户态可解；2026-10-09 的"用户态解不开"作废）**：把学生端安装的 `WH_KEYBOARD_LL` / `WH_MOUSE_LL` / `WH_KEYBOARD` / `WH_GETMESSAGE` 等钩子**替换成空过程**（v0.6.11 起；实现细节见 docs\原理与开发史.md），并拦下 `ClipCursor` / `BlockInput` / 逃生类热键 / SPI 改写；只处理**确实是远志自己装的**钩子与逃生组合键，不动同进程里其它组件的合法钩子/热键。**但机房实测证明它解不开封锁**：客户端进程内一次都没调过这些 API、内核 clip 矩形在整段广播期都是整屏，鼠标却仍被反复拽回左上角（当时判为“设备层封锁”，完整清单第 14 条）。注意：这条结论的依据已被 2026-10-15 的三个本机实验推翻一半——“低层钩子没收到”不能推出“设备层”，详见本文件已知限制第 8 条。这条开关现在的实际价值是“中和钩子 + 留下证据”。
**2026-10-16 上午机房实测（机房某台学生机，D:\YZ，08:44→10:14）把这条旧结论推翻了**：
① 主程序日志出现 **约 1000 行（去重后）** `[输入取证]`，客户端进程从 08:50:16 起 **每秒 10~14 次** `SetCursorPos …（最后一次 500,0）` 并伴随 `mouse_event`
⇒ **把光标摁回角落的就是学生端进程自己的用户态调用**，不是设备层；
② 键鼠探针在**用户确认"被锁"的那段时间**里，每一次物理按键变化**都被我们的低层钩子看到**（"钩子全无 0 次"，探针每秒重装钩子共 954 次，始终站在钩子链头），
光标"离开左上角"的自由段 **83 段、中位 504 ms**、最远 2202 px ⇒ 与"能动能点、几百毫秒后被拽回"的手感吻合。
⇒ 结论：**拦截发生在用户态**，拔钩/拦截这条路有机会解；具体拦截实现待改。
  **2026-10-15 现场状态**：那天早上在机房跑键鼠探针时，`tools\input-probe.ps1` 一传 `-TargetProc` 就崩
  （脚本里 `$P = [YzInputProbe]` 被后面的 `$p = Get-Process` 覆盖，变量名大小写不敏感）⇒ **A/B/C/D 四节一条数据都没拿到**，
  所以 10-09 的判决仍是当前最好的结论，但**尚未被 A/B/C/D 复核过**。脚本已在 v0.6.41 修好（并专门用
  `-TargetProc` 实测通过），A/B/C/D 的现场复核列为待办。**在拿到 A/B/C/D 之前，不要对外声称"已确认是设备层"。**
* **防监视（默认关闭）**：开启后教师端“监视/远程桌面”看到的是开启瞬间的静止画面（冻结帧），关闭立即恢复实时。**但它目前只覆盖"学生端进程自己用 GDI 抓屏"这一条路径**：厂商的采集层还有 DXGI（`ExdDtDup.dll`）与独立进程的 GDI helper（`ScreenRecoder.exe`），那两条路上这个开关无效——原因与后续计划见本文件已知限制第 1 条。
* **只拦"把光标摁回角落"的调用（默认关闭，v0.6.47）**（`Flags` 的 `0x40` 位）：它**不动教师的正常遥控/协助**，
  只在检测到"**同一个目标点在 1 秒内被重复调用 ≥8 次**"时丢弃那些调用，并把该点记成"认定点"（滑动 TTL 5 秒，期内同点一律丢）。
  依据：2026-10-16 机房日志去重后 954 条带坐标取证里，**950 行是同一个边缘点 `(500,0)`、每秒 10~14 次**，
  而"内部移动"16 分钟只有 5 次 ⇒ 两者在"同点重复频率"这个维度上分得开。本机测试形态（每 3 秒一次内点）实测丢弃率 **92%**，
  其余是学习窗口与内点调用；机房那种"长时间只摁一个点"的形态下接近 100%。
  与 `0x10` 的关系：`0x10` 仍是"全部丢弃"且优先级更高，`0x40` 只丢归位类，两者互不干扰。
* **拦截教师端遥控输入（默认关闭）**（`Flags` 的 `0x10` 位）：开启后丢弃学生端进程内的 `SendInput` / `mouse_event` / `keybd_event` / `SetCursorPos`，教师端无法接管你的鼠标键盘。**2026-10-16 机房实测要点**：全屏广播/锁键鼠期间"光标能被移动、但几百毫秒就被拽回角落"的现象，就是学生端进程自己每秒 10~14 次调用 `SetCursorPos` 造成的（主程序 `[输入取证]` 约 1000 行（去重后） + 键鼠探针两条独立证据）；**这一项不开，那些调用会被原样转发、光标就自由不了**。开了的代价是教师端的遥控/远程协助也会被一起挡掉——想在"自由移动鼠标"与"保留协助"之间两全，需要更细的判据（尚未实现）。
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
  tests/run_sim_test.ps1    本地自动回归脚本（二十九个阶段 75 项断言，约 4.3 分钟（v0.6.48 实测 255.0 秒；基线 383.6 秒，-33.5%），需要管理员权限），
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

## 快速上手

```powershell
# 构建（VS 在 D:\VisualStudioApp、Windows SDK 在 D:\Windows Kits\10；build.ps1 用 vswhere 定位，不写死路径）
pwsh -File build.ps1 -Both          # 交付构建：dist\Win32 与 dist\x64（两平台版本一致）
pwsh -File build.ps1 -NoElevate     # 免提权测试构建：只写 build\noelev\（asInvoker，方便本地调试宿主侧）

# 本地回归（需要管理员；29 个阶段 / 75 项断言 / 约 4.3 分钟）
pwsh -File tests\run_sim_test.ps1
```

**机房现场怎么用**见本地清单 `工作顺序.txt`（不入库）：U 盘目录布局、必须用 `YZSysRun.exe` 以 SYSTEM 启动、
出发前一把过的 `tools\preflight-check.ps1`、键鼠探针与输入取证、以及走人前的回收脚本。

---

## 实现要点

一句话：把一颗 DLL 注入学生端进程（或它拉起的抓屏子进程），**把厂商自己装的钩子换成空过程**、
把窗口从全屏广播里"摘"出来、并在需要的进程里冻住 DXGI 桌面复制；所有功能通过命名管道与一个托盘主程序配合，
按配置位（`Flags`）逐项开关，遇到考试/测验强信号立即全部停用。

**完整实现细节、证据链、以及逐轮开发史见 [`docs/原理与开发史.md`](docs/原理与开发史.md)。**

---

## 已知限制（摘要，完整证据见 docs）

1. **防监视只覆盖 GDI 路径** ✗：厂商采集层还有 DXGI（`KsFiles\ExdDtDup.dll`）与独立进程的 GDI helper
   （`ScreenRecoder.exe`），那两条路上"冻结"无效 ⇒ **防监视保持默认关闭**；v0.6.32 起改为"识别抓屏子进程 →
   按 PID 注入 → 在那个进程里冻 DXGI"，落地效果以机房实测为准。**v0.6.45 起修掉了一个会让抓屏子进程里钩子完全不启用的宿主闸门问题**（2026-10-16 机房实测发现：注入报"成功"但子进程一行日志都没有），效果待下次机房复核。
2. **SYSTEM 与管理员差一级完整性**：学生端是 SYSTEM、我们默认是管理员 ⇒ 跨进程注入/消息钩子/改窗口会被
   UIPI 挡住。**部署硬要求：用 `YZSysRun.exe -- YZTrainer.exe` 以 SYSTEM 启动**（详见本地清单 `工作顺序.txt`，不入库）。
3. **假全屏与窗口化互斥**，同时打开时以假全屏为准。
4. **托盘图标不能声称"固定 GUID 一定生效"**：GUID 注册成败与环境有关（现场实测成功、本机有时回落 uID）。
   真正消除"死图标"的是**优雅退出**（v0.6.34 起回归 18 处都走托盘退出）；v0.6.41 起还会在显示窗口/解除冻结时
   **重挂一次托盘图标**，避免"图标被 Explorer 收走后拿不回来"。
5. **抓屏子进程识别靠特征匹配**（进程名 / 模块名 `ExdDtDup.dll` 一类 / 远志安装路径）⇒ 厂商改命名就会失效，
   这是"防监视在机房能不能用"的前提。
6. **采集栈扫描默认 30 秒一轮**（`CaptureScanIntervalMs`）：现场启动监视后最多等 30 秒才出现"抓屏子进程出现"是正常的。
7. **`EnableExamGuard=0` 仅供调试**，不得作为交付默认值；考试/测验强信号命中即全功能停用。
8. **"低层钩子没收到"≠"设备/驱动层拦截"**（2026-10-15 三个本机实验，最重要的一条判据纪律）：

9. **被强杀的实例会在通知区留下一个"死图标"**（要鼠标划过才消失）：托盘图标的清理只能靠进程自己执行，被 `Stop-Process`/任务管理器结束后就没人调 `NIM_DELETE` 了。
   本机实测（2026-10-17）：**提权运行时**用固定 GUID 注册成功、退出时按 GUID 删除成功（`托盘图标: 退出前清理（GUID 身份=成功 / uID 身份=失败）`），
   所以正常"开→关"不会累积；**免提权**运行时 GUID 注册会失败并回落 uID（每个实例占一个槽位），这时被强杀就会留一个残影。回归测试已断言"本轮改强杀次数必须为 0"。
   远志在学生端进程里装的 `WH_KEYBOARD_LL` 只要不调 `CallNextHookEx` 就能把事件吞掉，**我们同样收不到**；
   实测 ① 钩子吞掉之后 `GetAsyncKeyState` 依然是 0，② 低层钩子的调用顺序是"**后装的先被调用**"
   （先装 A 后装 B，序列 `BABABA…`），③ Raw Input（`WM_INPUT`，`RIDEV_INPUTSINK`）在钩子吞事件时同样收不到。
   ⇒ **用户态可见的一切都在钩子链下游**，单靠探测无法区分"设备层"与"更早的用户态钩子吞掉"，
   **不要凭"钩子计数为 0"下结论**。可用办法只有两个：探针每秒**重装**自己的钩子以站在链头
   （`tools\input-probe.ps1 -WatchMinutes`），或看学生端进程自己的 `[输入取证] … SetCursorPos N 次 …`
   日志（v0.6.43 起，只记录不拦截）——后者能直接回答"周期性把光标拽回左上角的是不是它自己"。
   另外：`## 它做什么` 里那条旧结论（2026-10-09 判"用户态解不开"）**已被 2026-10-16 机房的键鼠探针
   与主程序 `[输入取证]` 共同推翻**——被锁期间事件照样到达用户态（"钩子全无 0 次"），而光标是学生端
   进程自己每秒 10~14 次 `SetCursorPos(500,0)` 按住的 ⇒ **用户态可解**，详见 `## 它做什么` 那一条。

---

## 回归测试

`tests\run_sim_test.ps1`：**29 个阶段 / 75 项断言**，本机实测约 **4.3 分钟**（v0.6.48 为 255.0 秒；
优化前基线 383.6 秒）。它用自带的模拟目标复现机房现象（全屏广播、考试窗口、抓屏子进程、晚加载钩子、
反横跳、身份不足、DXGI 取证、输入取证等），逐阶段打印 `[计时] 阶段X 用时 N 秒` 便于定位回归。
**逐阶段断言清单以脚本头部说明为准**（`tests\run_sim_test.ps1` 开头逐阶段列出断言条数）；各阶段的来龙去脉与逐轮开发史见 [`docs/原理与开发史.md`](docs/原理与开发史.md)。

---

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


## 免责声明与使用边界

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
