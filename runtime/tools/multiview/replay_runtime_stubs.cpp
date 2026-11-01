// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt_core also contains the game runtime. The replay binary links its
// renderer and memory objects, but never initializes recompiled game modules.
#include "guest_abi.h"
#include "runtime.h"

namespace nxrt {
RuntimeConfig g_config;
}

extern "C" {
const RecompStaticModule* suyu_recomp_static_modules_v4(unsigned* count) {
    if (count) *count = 0;
    return nullptr;
}
int suyu_recomp_static_guard_v2(unsigned) { return 0; }
int suyu_recomp_static_fastmem_v1(uint32_t, uint32_t, uint64_t, uint32_t, uint32_t) { return 0; }
unsigned suyu_recomp_static_features_v1(void) { return 0; }
unsigned suyu_recomp_static_guard_gen_v1(uint32_t, RecompGuardGenModule*, unsigned) { return 0; }
unsigned suyu_recomp_static_fpx_v1(uint32_t, uint32_t, uint64_t) { return 0; }
}
