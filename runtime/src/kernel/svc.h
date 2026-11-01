// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

namespace nxrt {

class KThread;

// Services the SVC `imm` for `t`: arguments in x0-x7, results written back.
void CallSvc(KThread* t, u32 imm);

// Inventory of SVC use, printed at exit when NXRT_SVC_STATS is set.
void DumpSvcStats();

} // namespace nxrt
