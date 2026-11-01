// SPDX-License-Identifier: GPL-3.0-or-later
// Registers every service this runtime implements with sm:.
#include "services/common.h"
#include "services/nvdrv.h"
#include "services/vi.h"
#include "services/hid.h"

namespace nxrt::hle {

void RegisterAllServices() {
    RegisterMiscServices();
    RegisterAmServices();
    RegisterTimeServices();
    RegisterSettingsService();
    RegisterFsServices();
    RegisterAccServices();
    RegisterMiiServices();
    RegisterAudioServices();
    RegisterPlServices();
    RegisterNvdrvServices();
    RegisterViServices();
    RegisterHidServices();
}

} // namespace nxrt::hle
