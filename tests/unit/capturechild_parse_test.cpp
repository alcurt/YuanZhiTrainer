// 抓屏子进程标记解析的单元测试（v0.6.32）。不需要管理员、不依赖任何进程。
#include "capturechild.h"

#include <stdio.h>

static int g_fail = 0;

static void Check(const wchar_t* name, const wchar_t* line, bool wantOk, DWORD wantPid)
{
    DWORD pid = 0;
    const bool ok = yzcap::ParseCaptureChildMarker(line != nullptr ? line : L"", &pid);
    const bool pass = (ok == wantOk) && (!wantOk || pid == wantPid);
    if (!pass)
    {
        ++g_fail;
        printf("[FAIL] %ls: ok=%d pid=%u  (期望 ok=%d pid=%u)\n", name, ok ? 1 : 0, pid,
               wantOk ? 1 : 0, wantPid);
    }
    else
    {
        printf("[ ok ] %ls\n", name);
    }
}

int main()
{
    Check(L"机房审计行（前缀很长）",
          L"[2026-10-12 12:48:08.359] pid=9928 tid=8532 kind=CreateProcessW origin=remote ok pid=8632 [抓屏子进程 pid=8632]",
          true, 8632);
    Check(L"标记后还有内容", L"xxx [抓屏子进程 pid=24704] yyy", true, 24704);
    Check(L"没有标记", L"[2026-10-12] pid=9928 kind=CreateProcessW ok pid=8632", false, 0);
    Check(L"pid=0", L"[抓屏子进程 pid=0]", false, 0);
    Check(L"非数字", L"[抓屏子进程 pid=abc]", false, 0);
    Check(L"没有右括号", L"[抓屏子进程 pid=1234", false, 0);
    Check(L"空串", L"", false, 0);
    Check(L"空数字", L"[抓屏子进程 pid=]", false, 0);
    Check(L"超过 DWORD", L"[抓屏子进程 pid=4294967296]", false, 0);
    Check(L"DWORD 上限", L"[抓屏子进程 pid=4294967295]", true, 4294967295u);
    Check(L"两个标记取第一个", L"[抓屏子进程 pid=111] [抓屏子进程 pid=222]", true, 111);
    Check(L"前后有空格和冒号", L"抓屏子进程出现: pid=999  [抓屏子进程 pid=999]", true, 999);

    printf(g_fail == 0 ? "\n全部通过\n" : "\n失败 %d 项\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}