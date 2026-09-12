#pragma once
#include <windows.h>

#include "yz_protocol.h"

namespace yzhook
{
void EngineStart();                 /* 由 DllMain 在工作线程上调用 */
void EngineStop();

bool EngineIsTargetHost();

void EngineApplyConfig(const YZ_CONFIG& cfg);
void EngineFillStatus(YZ_STATUS* status);

bool EngineIsExamMode();
void EngineSetFlag(DWORD flag, bool on);
} /* namespace yzhook */
