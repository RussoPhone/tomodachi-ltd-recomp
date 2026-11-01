// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "services/nvdrv.h"

namespace {

struct SubmitGpfifo {
    u64 address;
    u32 num_entries;
    u32 flags;
    s32 fence_id;
    u32 fence_value;
};

struct EventWait {
    s32 fence_id;
    u32 fence_value;
    s32 timeout_ms;
    u32 value;
};

static_assert(sizeof(SubmitGpfifo) == 24);
static_assert(sizeof(EventWait) == 16);

constexpr u32 kSuccess = 0;
constexpr u32 kTimeout = 5;
constexpr u32 kSubmitWithFenceIncrement = 0xC018481B;
constexpr u32 kEventWait = 0xC010001D;
constexpr u32 kTimeoutValueTag = 0x10000000;

u32 Submit(u32 gpu_fd, bool increment, SubmitGpfifo* result = nullptr) {
    SubmitGpfifo packet{};
    packet.flags = increment ? 2 : 0;
    std::vector<u8> io(sizeof(packet));
    std::memcpy(io.data(), &packet, sizeof(packet));
    std::vector<u8> inline_out;
    const u32 status = nxrt::hle::DispatchNvdrvIoctl(
        gpu_fd, kSubmitWithFenceIncrement, io, inline_out);
    std::memcpy(&packet, io.data(), sizeof(packet));
    if (result) *result = packet;
    return status;
}

u32 Wait(u32 ctrl_fd, s32 id, u32 target, s32 timeout_ms,
         EventWait* result = nullptr) {
    EventWait packet{id, target, timeout_ms, 0};
    std::vector<u8> io(sizeof(packet));
    std::memcpy(io.data(), &packet, sizeof(packet));
    std::vector<u8> inline_out;
    const u32 status = nxrt::hle::DispatchNvdrvIoctl(
        ctrl_fd, kEventWait, io, inline_out);
    std::memcpy(&packet, io.data(), sizeof(packet));
    if (result) *result = packet;
    return status;
}

} // namespace

int main() {
    const u32 gpu_fd = nxrt::hle::OpenNvdrvDevice("/dev/nvhost-gpu");
    const u32 ctrl_fd = nxrt::hle::OpenNvdrvDevice("/dev/nvhost-ctrl");

    // A GPU submit with the driver's increment flag returns the promised fence.
    SubmitGpfifo submitted{};
    if (Submit(gpu_fd, true, &submitted) != kSuccess) return 1;
    if (submitted.fence_id <= 0 || submitted.fence_value != 2) return 2;

    // A completed fence is observable through the ioctl output fields.
    EventWait completed{};
    if (Wait(ctrl_fd, submitted.fence_id, submitted.fence_value, 0, &completed) != kSuccess)
        return 3;
    if (completed.value != submitted.fence_value) return 4;

    // A finite wait for a future value times out and returns the encoded event value.
    const u32 future = submitted.fence_value + 1;
    EventWait timed_out{};
    if (Wait(ctrl_fd, submitted.fence_id, future, 5, &timed_out) != kTimeout) return 5;
    if (timed_out.value != ((static_cast<u32>(submitted.fence_id) << 16) |
                            kTimeoutValueTag)) return 6;

    // A waiter must wake when a later submit advances and completes this syncpoint.
    const u32 wake_target = submitted.fence_value + 2;
    std::atomic<bool> waiter_started = false;
    std::atomic<bool> waiter_done = false;
    std::atomic<u32> waiter_status = kTimeout;
    EventWait wake_result{};
    std::thread waiter([&] {
        waiter_started = true;
        waiter_status = Wait(ctrl_fd, submitted.fence_id, wake_target, 2000, &wake_result);
        waiter_done = true;
    });
    while (!waiter_started.load()) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (waiter_done.load()) {
        waiter.join();
        return 7;
    }
    SubmitGpfifo later{};
    if (Submit(gpu_fd, true, &later) != kSuccess) {
        waiter.join();
        return 8;
    }
    waiter.join();
    if (!waiter_done.load() || waiter_status.load() != kSuccess) return 9;
    if (later.fence_id != submitted.fence_id || later.fence_value != wake_target) return 10;
    if (wake_result.value != wake_target) return 11;
    return 0;
}
