#pragma once
//
// 采集栈监控（主程序侧常驻）：教师监视/缩略图时，厂商会在某个进程里加载
// ExdDtDup.dll（DXGI 桌面复制）或拉起 ScreenRecoder.exe（GDI 抓屏 helper）。
// "冻结画面"要钩哪条路取决于这件事，而它只在教师动手的那几秒里出现——
// 靠人工在那一刻跑探针赌不起（10-09 就赌输了）。所以在主程序里挂一个低频常驻扫描：
//   * 每 30 秒扫一轮（远志相关进程 + 新出现 30 秒内的进程 + 每 5 分钟一次全进程兜底）
//   * 第一次发现采集组件就记日志、并把当前状态自动导出一份诊断包
//   * 诊断包的 snapshot.txt 里带一块"采集栈快照"（capturewatch::Dump()）
//
#include <string>

namespace capturewatch
{
/* 看门狗线程每秒调用；内部自己按 30 秒节流 */
void Tick();

/* 供诊断包快照使用的文本块（会对当前时刻做一次扫描，较慢但导出本身很少发生） */
std::wstring Dump();

/* 清空累计状态（目前只有测试用得上） */
void Reset();
} /* namespace capturewatch */
