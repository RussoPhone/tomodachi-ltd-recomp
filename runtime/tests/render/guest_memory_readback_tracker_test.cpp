#include <render/guest_memory_range.h>

#include <cstdint>
#include <cstdlib>

int main() {
    nxrt::render::GuestMemoryReadbackTracker tracker;
    tracker.Record({0x2044dd6380, 100, {0x1000, 0x40}});

    const auto same_frame = tracker.FindOverlappingWrites(100, {0x1020, 0x10});
    if (same_frame.size() != 1 || same_frame[0].program != 0x2044dd6380 || same_frame[0].frame != 100) {
        std::abort();
    }
    if (!tracker.FindOverlappingWrites(100, {0x1040, 0x10}).empty()) std::abort();
    if (tracker.FindOverlappingWrites(101, {0x1000, 1}).size() != 1) std::abort();
    if (!tracker.FindOverlappingWrites(102, {0x1000, 1}).empty()) std::abort();

    tracker.Record({0x2044e133c8, 101, {0x1000, 0x40}});
    const auto latest = tracker.FindOverlappingWrites(101, {0x1000, 0x40});
    if (latest.size() != 1 || latest[0].program != 0x2044e133c8 || latest[0].frame != 101) std::abort();

    tracker.Record({0x200b2d1db0, 102, {0x2000, 0}});
    if (!tracker.FindOverlappingWrites(102, {0x2000, 1}).empty()) std::abort();
}
