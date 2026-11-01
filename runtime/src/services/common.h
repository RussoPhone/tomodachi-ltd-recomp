// SPDX-License-Identifier: GPL-3.0-or-later
// Shared bits for service implementations.
#pragma once

#include <array>
#include <memory>

#include "hle/ipc.h"

namespace nxrt::hle {

// The one local user account this runtime presents.
constexpr std::array<u64, 2> kUserId{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};

// Returns a handle-able event that stays alive with the service.
inline std::shared_ptr<KEvent> MakeEvent(const char* name, bool signaled = false) {
    auto e = std::make_shared<KEvent>(name);
    e->signaled = signaled;
    return e;
}

// IStorage: a byte buffer passed between the game and applets.
class IStorage final : public ServiceObject {
public:
    explicit IStorage(std::vector<u8> data);
    std::vector<u8> data;
};

void RegisterAmServices();
void RegisterMiscServices();
void RegisterTimeServices();
void RegisterSettingsService();
void RegisterFsServices();
void RegisterAccServices();
void RegisterMiiServices();
void RegisterAudioServices();
void RegisterPlServices();

} // namespace nxrt::hle
