#pragma once
#include <windows.h>

namespace yzhook
{
/* 教师端远程执行审计：只记录，不拦截。
   覆盖学生端进程内的 CreateProcessA/W、WinExec、ShellExecuteExW、ExitWindowsEx。 */
bool  ExecHooksInstall(bool enableNow);
void  ExecHooksShutdown();

DWORD ExecAuditCount();
} /* namespace yzhook */
