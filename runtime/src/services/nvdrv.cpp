// SPDX-License-Identifier: GPL-3.0-or-later
// nvdrv without a GPU. The game's NVN driver (guest code) talks to these device
// nodes; GPFIFO submissions execute inline, and syncpoints complete only after
// their CPU-visible effects have been applied.
// Memory handles and GPU address mappings are tracked for real, because the
// renderer that replaces this will need them (GpuVaToGuest).
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

#include "gpu/null_gpu.h"
#include "memory.h"
#include "services/common.h"
#include "services/nvdrv.h"
#include "services/syncpoint_timeline.h"

namespace nxrt::hle {

namespace {

enum NvResult : u32 {
    kNvSuccess = 0,
    kNvNotImplemented = 1,
    kNvBadParameter = 4,
    kNvTimeout = 5,
    kNvFileOperationFailed = 0x30003,
    kNvConfigVarNotFound = 0x30006,
};

struct NvFence {
    s32 id;
    u32 value;
};

struct NvmapHandle {
    u32 size = 0;
    u32 align = 0;
    u8 kind = 0;
    u64 address = 0; // guest address backing it, 0 until allocated
    u32 id = 0;
};

struct GpuMapping {
    u64 guest;
    u64 size;
};

struct NvState {
    std::mutex mutex;
    std::mutex submit_mutex;
    std::map<u32, NvmapHandle> handles; // handle id == handle value
    u32 next_handle = 1;
    std::map<u64, GpuMapping> gpu_map; // GPU VA -> guest memory
    u64 next_small_va = 0x8000000;     // small pages, below the split
    u64 next_big_va = u64{1} << 34;    // big pages
    SyncpointTimeline syncpoints;
    u32 next_syncpoint = 1;
    std::map<u32, std::string> fds;
    std::map<u32, u32> channel_syncpoint; // channel fd -> syncpoint id
    u32 next_fd = 1;
    std::shared_ptr<KEvent> events[64];
};

NvState& State() {
    static NvState s;
    return s;
}

template <typename T>
T& As(std::vector<u8>& buf) {
    if (buf.size() < sizeof(T)) buf.resize(sizeof(T));
    return *reinterpret_cast<T*>(buf.data());
}

u64 AllocVa(u64 size, u32 page_size) {
    NvState& s = State();
    const bool big = page_size >= 0x10000 || size >= 0x100000;
    u64& next = big ? s.next_big_va : s.next_small_va;
    const u64 align = big ? 0x20000 : 0x1000;
    next = (next + align - 1) & ~(align - 1);
    const u64 va = next;
    next += (size + align - 1) & ~(align - 1);
    return va;
}

// --------------------------------------------------------------- devices

u32 Nvmap(u32 nr, std::vector<u8>& io) {
    NvState& s = State();
    std::lock_guard lock(s.mutex);
    switch (nr) {
    case 0x01: { // Create
        struct P { u32 size, handle; };
        P& p = As<P>(io);
        const u32 h = s.next_handle++;
        s.handles[h] = {p.size, 0, 0, 0, h};
        p.handle = h;
        return kNvSuccess;
    }
    case 0x03: { // FromId
        struct P { u32 id, handle; };
        P& p = As<P>(io);
        p.handle = s.handles.count(p.id) ? p.id : 0;
        return p.handle ? kNvSuccess : kNvBadParameter;
    }
    case 0x04: { // Alloc
        struct P { u32 handle, heap_mask, flags, align; u8 kind; u8 pad[7]; u64 address; };
        P& p = As<P>(io);
        auto it = s.handles.find(p.handle);
        if (it == s.handles.end()) return kNvBadParameter;
        it->second.align = p.align;
        it->second.kind = p.kind;
        it->second.address = p.address;
        return kNvSuccess;
    }
    case 0x05: { // Free
        struct P { u32 handle; u32 pad; u64 address; u32 size; u32 flags; };
        P& p = As<P>(io);
        auto it = s.handles.find(p.handle);
        if (it == s.handles.end()) return kNvBadParameter;
        p.address = it->second.address;
        p.size = it->second.size;
        p.flags = 0;
        s.handles.erase(it);
        return kNvSuccess;
    }
    case 0x09: { // Param
        struct P { u32 handle, param, result; };
        P& p = As<P>(io);
        auto it = s.handles.find(p.handle);
        if (it == s.handles.end()) return kNvBadParameter;
        switch (p.param) {
        case 1: p.result = it->second.size; break;   // size
        case 2: p.result = it->second.align; break;  // alignment
        case 3: p.result = 0x40000000; break;        // base (unused)
        case 4: p.result = 0; break;                 // heap
        case 5: p.result = it->second.kind; break;   // kind
        default: p.result = 0; break;
        }
        return kNvSuccess;
    }
    case 0x0E: { // GetId
        struct P { u32 id, handle; };
        P& p = As<P>(io);
        p.id = p.handle;
        return kNvSuccess;
    }
    }
    LOG_WARN("nvmap: unknown ioctl 0x%x", nr);
    return kNvSuccess;
}

u32 AsGpu(u32 nr, std::vector<u8>& io) {
    NvState& s = State();
    std::lock_guard lock(s.mutex);
    switch (nr) {
    case 0x01: return kNvSuccess; // BindChannel
    case 0x02: {                  // AllocateSpace
        struct P { u32 pages, page_size, flags, pad; u64 offset; };
        P& p = As<P>(io);
        const u64 size = static_cast<u64>(p.pages) * p.page_size;
        if (!(p.flags & 1)) p.offset = AllocVa(size, p.page_size); // not a fixed offset
        return kNvSuccess;
    }
    case 0x03: return kNvSuccess; // FreeSpace
    case 0x05: {                  // UnmapBuffer
        struct P { s64 offset; };
        s.gpu_map.erase(static_cast<u64>(As<P>(io).offset));
        return kNvSuccess;
    }
    case 0x06: { // MapBufferEx
        struct P { u32 flags; u32 kind; u32 handle; u32 page_size; s64 buffer_offset; u64 mapping_size; s64 offset; };
        P& p = As<P>(io);
        auto it = s.handles.find(p.handle);
        if (it == s.handles.end()) return kNvBadParameter;
        const u64 size = p.mapping_size ? p.mapping_size : it->second.size;
        if (p.flags & 0x100) { // remap within an existing mapping
            return kNvSuccess;
        }
        if (!(p.flags & 1)) p.offset = static_cast<s64>(AllocVa(size, p.page_size));
        s.gpu_map[static_cast<u64>(p.offset)] = {it->second.address + static_cast<u64>(p.buffer_offset), size};
        return kNvSuccess;
    }
    case 0x08: { // GetVaRegions
        struct Region { u64 offset; u32 page_size; u32 pad; u64 pages; };
        struct P { u64 buf_addr; u32 buf_size; u32 reserved; Region regions[2]; };
        P& p = As<P>(io);
        p.buf_size = 2 * sizeof(Region);
        p.regions[0] = {0x8000000, 0x1000, 0, ((u64{1} << 34) - 0x8000000) / 0x1000};
        p.regions[1] = {u64{1} << 34, 0x20000, 0, ((u64{1} << 37) - (u64{1} << 34)) / 0x20000};
        return kNvSuccess;
    }
    case 0x09: return kNvSuccess; // AllocAsEx
    case 0x14: {                  // Remap: entries of 20 bytes
        struct E { u16 flags, kind; u32 handle, handle_offset_big_pages, as_offset_big_pages, big_pages; };
        for (size_t off = 0; off + sizeof(E) <= io.size(); off += sizeof(E)) {
            E e;
            std::memcpy(&e, io.data() + off, sizeof(E));
            const u64 va = static_cast<u64>(e.as_offset_big_pages) << 16;
            const u64 size = static_cast<u64>(e.big_pages) << 16;
            auto it = s.handles.find(e.handle);
            if (e.handle && it != s.handles.end()) {
                s.gpu_map[va] = {it->second.address + (static_cast<u64>(e.handle_offset_big_pages) << 16), size};
            } else {
                s.gpu_map.erase(va);
            }
        }
        return kNvSuccess;
    }
    }
    LOG_WARN("nvhost-as-gpu: unknown ioctl 0x%x", nr);
    return kNvSuccess;
}

u32 CtrlGpu(u32 nr, std::vector<u8>& io, std::vector<u8>& inline_out) {
    switch (nr) {
    case 0x01: As<u32>(io) = 1; return kNvSuccess; // ZCullGetCtxSize
    case 0x02: {                                    // ZCullGetInfo
        const u32 info[10] = {0x20, 0x20, 0x400, 0x800, 0x20, 0x20, 0xc0, 0x20, 0x40, 0x10};
        io.resize(std::max<size_t>(io.size(), sizeof(info)));
        std::memcpy(io.data(), info, sizeof(info));
        return kNvSuccess;
    }
    case 0x03: case 0x04: case 0x07: return kNvSuccess; // ZBC set/query, FlushL2
    case 0x05: { // GetCharacteristics
        io.resize(std::max<size_t>(io.size(), 16 + 160));
        u64 size = 0xA0, addr = 0xdeadbeef;
        std::memcpy(io.data(), &size, 8);
        std::memcpy(io.data() + 8, &addr, 8);
        u8* gc = io.data() + 16;
        auto w32 = [&](size_t off, u32 v) { std::memcpy(gc + off, &v, 4); };
        auto w64 = [&](size_t off, u64 v) { std::memcpy(gc + off, &v, 8); };
        w32(0x00, 0x120); w32(0x04, 0xb); w32(0x08, 0xa1); w32(0x0c, 1);
        w64(0x10, 0x40000); w64(0x18, 0);
        w32(0x20, 2); w32(0x24, 0x20); w32(0x28, 0x20000); w32(0x2c, 0x20000);
        w32(0x30, 0x1B); w32(0x34, 0x30000); w32(0x38, 1); w32(0x3c, 0x503);
        w32(0x40, 0x503); w32(0x44, 0x80); w32(0x48, 0x28); w32(0x4c, 0);
        w64(0x50, 0x55);
        w32(0x58, 0x902D); w32(0x5c, 0xB197); w32(0x60, 0xB1C0); w32(0x64, 0xB06F);
        w32(0x68, 0xA140); w32(0x6c, 0xB0B5); w32(0x70, 1); w32(0x74, 0);
        w32(0x78, 2); w32(0x7c, 1); w32(0x80, 0); w32(0x84, 1);
        w32(0x88, 0x21D70); w32(0x8c, 0);
        w64(0x90, 0x6230326D67ULL); w64(0x98, 0);
        inline_out.assign(gc, gc + 160);
        return kNvSuccess;
    }
    case 0x06: { // GetTPCMasks
        struct P { u32 mask_buffer_size, pad; u64 mask_buffer_address; u32 tpc_mask, pad2; };
        P& p = As<P>(io);
        if (p.mask_buffer_size) p.tpc_mask = 3;
        inline_out.assign(4, 0);
        inline_out[0] = 3;
        return kNvSuccess;
    }
    case 0x14: { // GetActiveSlotMask
        struct P { u32 slot, mask; };
        As<P>(io) = {7, 1};
        return kNvSuccess;
    }
    case 0x13: return kNvNotImplemented;              // VsmsMapping (as suyu)
    case 0x15: As<u32>(io) = 100; return kNvSuccess; // PmuGetGpuLoad
    case 0x1C: {                                      // GetGpuTime
        As<u64>(io) = ReadCntpct() * 625 / 12;
        return kNvSuccess;
    }
    }
    LOG_WARN("nvhost-ctrl-gpu: unknown ioctl 0x%x", nr);
    return kNvSuccess;
}

u32 AllocSyncpoint() {
    NvState& s = State();
    return s.next_syncpoint < 192 ? s.next_syncpoint++ : 191;
}

u32 Gpu(u32 fd, u32 group, u32 nr, std::vector<u8>& io) {
    NvState& s = State();
    std::unique_lock submit_lock(s.submit_mutex, std::defer_lock);
    const bool legacy_submit = group == 0 && (nr == 0x08 || nr == 0x1b);
    const bool nvhost_submit = group == 'H' && nr == 0x1b;
    if (legacy_submit || nvhost_submit) submit_lock.lock();
    std::unique_lock lock(s.mutex);
    u32& sp = s.channel_syncpoint[fd];
    if (!sp) sp = AllocSyncpoint();
    if (group == 0 && nr == 0x03) return kNvSuccess; // GetWaitbase
    if (group == 'G') return kNvSuccess;              // client data
    switch (nr) {
    case 0x01: case 0x03: case 0x0b: case 0x0c: case 0x0d: case 0x1d:
        return kNvSuccess; // nvmap fd, timeout, zcull, error notifier, priority, timeslice
    case 0x17:
        return kNvSuccess; // polled every 100 ms per channel: no errors to report
    case 0x08:
    case 0x1b: { // SubmitGpfifo
        struct P { u64 address; u32 num_entries; u32 flags; NvFence fence; };
        P& p = As<P>(io);
        u32 incr = 0;
        if (p.flags & 2) incr = 2; // fence_increment: the driver's wait-for-idle + increment
        const auto fence = s.syncpoints.Submit(sp, incr);
        p.fence = {static_cast<s32>(sp), fence.value};
        // Effects still run inline; completion is published only after they
        // have all been applied.
        const size_t n = std::min<size_t>(p.num_entries, (io.size() - sizeof(P)) / 8);
        std::vector<u64> entries(n);
        if (n) std::memcpy(entries.data(), io.data() + sizeof(P), n * 8);
        lock.unlock();
        gpu::SubmitGpfifo(fd, entries.data(), entries.size());
        s.syncpoints.Complete(fence);
        return kNvSuccess;
    }
    case 0x09: { // AllocObjCtx
        struct P { u32 class_num, flags; u64 obj_id; };
        As<P>(io).obj_id = 0;
        return kNvSuccess;
    }
    case 0x18:
    case 0x1a: { // AllocGpfifoEx(2)
        struct P { u32 num_entries, num_jobs, flags; NvFence fence; u32 reserved[3]; };
        As<P>(io).fence = {static_cast<s32>(sp), s.syncpoints.Submitted(sp)};
        return kNvSuccess;
    }
    }
    LOG_WARN("nvhost-gpu: unknown ioctl %c/0x%x", group ? group : '0', nr);
    return kNvSuccess;
}

u32 Ctrl(u32 nr, std::vector<u8>& io) {
    NvState& s = State();
    std::unique_lock lock(s.mutex);
    switch (nr) {
    case 0x1b: return kNvConfigVarNotFound; // NvOsGetConfigU32
    case 0x1c: return kNvSuccess;           // ClearEventWait
    case 0x1d:
    case 0x1e: { // Current routing characterization only; the 0x1e ABI contract is unverified.
        struct P { NvFence fence; s32 timeout; u32 value; };
        P& p = As<P>(io);
        if (p.fence.id < 0 || static_cast<u32>(p.fence.id) >= SyncpointTimeline::kCount) {
            return kNvBadParameter;
        }
        const u32 id = static_cast<u32>(p.fence.id);
        lock.unlock();
        const auto result = s.syncpoints.Wait(id, p.fence.value, p.timeout);
        if (result == SyncpointTimeline::WaitResult::Timeout) {
            p.value = (id << 16) | 0x10000000;
            return kNvTimeout;
        }
        p.value = s.syncpoints.Completed(id);
        return kNvSuccess;
    }
    case 0x1f: case 0x20: case 0x21: return kNvSuccess; // event register/unregister
    }
    LOG_WARN("nvhost-ctrl: unknown ioctl 0x%x", nr);
    return kNvSuccess;
}

u32 Dispatch(u32 fd, u32 command, std::vector<u8>& io, std::vector<u8>& inline_out) {
    std::string dev;
    {
        std::lock_guard lock(State().mutex);
        const auto it = State().fds.find(fd);
        if (it == State().fds.end()) return kNvBadParameter;
        dev = it->second;
    }
    const u32 nr = command & 0xFF;
    const u32 group = (command >> 8) & 0xFF;
    if (dev == "/dev/nvmap") return Nvmap(nr, io);
    if (dev == "/dev/nvhost-as-gpu") return AsGpu(nr, io);
    if (dev == "/dev/nvhost-ctrl-gpu") return CtrlGpu(nr, io, inline_out);
    if (dev == "/dev/nvhost-gpu") return Gpu(fd, group, nr, io);
    if (dev == "/dev/nvhost-ctrl") return Ctrl(nr, io);
    LOG_WARN("%s: ioctl 0x%08x ignored", dev.c_str(), command);
    return kNvSuccess;
}

class Nvdrv final : public ServiceObject {
public:
    explicit Nvdrv(const char* name) : ServiceObject(name) {
        Register(0, "Open", [](Ctx& ctx) {
            const std::string path = ctx.ReadString();
            const u32 fd = OpenNvdrvDevice(path);
            LOG_DEBUG("nvdrv: open %s -> fd %u", path.c_str(), fd);
            ctx.Push<u32>(fd);
            ctx.Push<u32>(kNvSuccess);
        });
        Register(1, "Ioctl", [](Ctx& ctx) { DoIoctl(ctx, false, false); });
        Register(2, "Close", [](Ctx& ctx) {
            const u32 fd = ctx.Pop<u32>();
            std::lock_guard lock(State().mutex);
            State().fds.erase(fd);
            ctx.Push<u32>(kNvSuccess);
        });
        Register(3, "Initialize", [](Ctx& ctx) { ctx.Push<u32>(kNvSuccess); });
        Register(4, "QueryEvent", [](Ctx& ctx) {
            ctx.Pop<u32>(); // fd
            const u32 event_id = ctx.Pop<u32>();
            auto& e = State().events[event_id & 63];
            if (!e) e = MakeEvent("nvdrv:event", true);
            ctx.PushCopyObject(e);
            ctx.Push<u32>(kNvSuccess);
        });
        Register(6, "GetStatus", [](Ctx& ctx) { ctx.Push<u32>(kNvSuccess); });
        Register(8, "SetAruid", [](Ctx& ctx) { ctx.Push<u32>(kNvSuccess); });
        Register(11, "Ioctl2", [](Ctx& ctx) { DoIoctl(ctx, true, false); });
        Register(12, "Ioctl3", [](Ctx& ctx) { DoIoctl(ctx, false, true); });
        Register(13, "SetGraphicsFirmwareMemoryMarginEnabled", [](Ctx&) {});
    }

    u16 PointerBufferSize() const override { return 0x8000; }

private:
    static void DoIoctl(Ctx& ctx, bool inline_in, bool inline_out) {
        const u32 fd = ctx.Pop<u32>();
        const u32 command = ctx.Pop<u32>();
        std::vector<u8> io = ctx.ReadBuffer(0);
        if (inline_in) {
            const std::vector<u8> extra = ctx.ReadBuffer(1);
            io.insert(io.end(), extra.begin(), extra.end());
        }
        const size_t out_size = ctx.OutBufferSize(0);
        if (io.size() < out_size) io.resize(out_size);
        std::vector<u8> extra_out;
        const u32 r = DispatchNvdrvIoctl(fd, command, io, extra_out);
        ctx.WriteBuffer(io.data(), std::min(io.size(), out_size), 0);
        if (inline_out && !extra_out.empty()) ctx.WriteBuffer(extra_out.data(), extra_out.size(), 1);
        ctx.Push<u32>(r);
    }
};

} // namespace

u32 OpenNvdrvDevice(const std::string& path) {
    NvState& s = State();
    std::lock_guard lock(s.mutex);
    const u32 fd = s.next_fd++;
    s.fds[fd] = path;
    return fd;
}

u32 DispatchNvdrvIoctl(u32 fd, u32 command, std::vector<u8>& io,
                       std::vector<u8>& inline_out) {
    return Dispatch(fd, command, io, inline_out);
}

bool GpuVaToGuest(u64 gpu_va, u64* guest) {
    NvState& s = State();
    std::lock_guard lock(s.mutex);
    auto it = s.gpu_map.upper_bound(gpu_va);
    if (it == s.gpu_map.begin()) return false;
    --it;
    if (gpu_va >= it->first + it->second.size) return false;
    *guest = it->second.guest + (gpu_va - it->first);
    return true;
}

u64 NvmapHandleAddress(u32 handle) {
    NvState& s = State();
    std::lock_guard lock(s.mutex);
    const auto it = s.handles.find(handle);
    return it == s.handles.end() ? 0 : it->second.address;
}

void RegisterNvdrvServices() {
    for (const char* name : {"nvdrv", "nvdrv:a", "nvdrv:s", "nvdrv:t"}) {
        RegisterService(name, [name] { return std::make_shared<Nvdrv>(name); });
    }
}

} // namespace nxrt::hle
