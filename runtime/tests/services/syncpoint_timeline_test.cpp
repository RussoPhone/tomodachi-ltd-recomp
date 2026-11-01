#include <atomic>
#include <chrono>
#include <thread>

#include "services/syncpoint_timeline.h"

int main() {
    nxrt::hle::SyncpointTimeline timeline;

    const auto first = timeline.Submit(7, 2);
    if (first.value != 2 || timeline.Completed(7) != 0) return 1;

    if (timeline.Wait(7, first.value, 0) != nxrt::hle::SyncpointTimeline::WaitResult::Timeout) return 2;
    timeline.Complete(first);
    if (timeline.Wait(7, first.value, 0) != nxrt::hle::SyncpointTimeline::WaitResult::Complete) return 3;

    const auto second = timeline.Submit(7, 1);
    std::atomic<bool> returned = false;
    std::thread waiter([&] {
        returned = timeline.Wait(7, second.value, 1000) == nxrt::hle::SyncpointTimeline::WaitResult::Complete;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (returned.load()) {
        waiter.join();
        return 4;
    }
    timeline.Complete(second);
    waiter.join();
    if (!returned.load() || timeline.Completed(7) != second.value) return 5;

    // Completing an older submission must never move a syncpoint backwards.
    timeline.Complete(first);
    if (timeline.Completed(7) != second.value) return 6;

    const auto pending = timeline.Submit(8, 1);
    if (timeline.Wait(8, pending.value, 1) != nxrt::hle::SyncpointTimeline::WaitResult::Timeout) return 7;

    timeline.Submit(9, 0xFFFFFFFEu);
    const auto wrapped = timeline.Submit(9, 3);
    if (wrapped.value != 1) return 8;
    timeline.Complete(wrapped);
    if (timeline.Wait(9, 0xFFFFFFFFu, 0) != nxrt::hle::SyncpointTimeline::WaitResult::Complete) return 9;
    return 0;
}
