#pragma once
#include <windows.h>

namespace yzhook
{
/* 只读自报：本进程的 HKCU 到底解析到哪个 hive（形如 \REGISTRY\USER\S-1-5-18）。
   这段代码跑在目标进程里，所以这是"第一视角"的事实，用来回答
   "某个策略值到底落在哪一家"。纯日志，不改任何状态。
   v0.6.13 起本模块**不再做任何策略写入**（原来那套备份/改写/还原经机房实测证明
   既无作用对象、又会污染默认用户模板），见 policy.cpp 顶部说明。 */
void PolicyLogCurrentUserHive();
} /* namespace yzhook */
