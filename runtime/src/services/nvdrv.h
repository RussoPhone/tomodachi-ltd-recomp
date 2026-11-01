// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

#include <string>
#include <vector>

namespace nxrt::hle {

// Guest address behind a GPU virtual address, as mapped through nvhost-as-gpu.
bool GpuVaToGuest(u64 gpu_va, u64* guest);
// Guest address an nvmap handle was allocated at (0 if unknown).
u64 NvmapHandleAddress(u32 handle);

void RegisterNvdrvServices();

// Narrow NVDRV device operations shared by the service handlers and integration tests.
u32 OpenNvdrvDevice(const std::string& path);
u32 DispatchNvdrvIoctl(u32 fd, u32 command, std::vector<u8>& io,
                       std::vector<u8>& inline_out);

} // namespace nxrt::hle
