// SPDX-License-Identifier: GPL-3.0-or-later
#include "hle/ipc.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include "cpu.h"
#include "memory.h"
#include "tts_trace.h"

namespace nxrt::hle {

namespace {

constexpr u32 kSfci = 0x49434653; // "SFCI"
constexpr u32 kSfco = 0x4F434653; // "SFCO"

enum CommandType : u32 {
    kInvalid = 0,
    kLegacyRequest = 1,
    kClose = 2,
    kLegacyControl = 3,
    kRequest = 4,
    kControl = 5,
    kRequestWithContext = 6,
    kControlWithContext = 7,
    kTipcClose = 15,
    kTipcFirst = 16,
};

std::mutex g_stats_mutex;
std::map<std::string, u64> g_stats;

void CountCommand(const std::string& service, const char* name, u32 id) {
    static const bool enabled = std::getenv("NXRT_IPC_STATS") != nullptr;
    if (!enabled) return;
    std::lock_guard lock(g_stats_mutex);
    char key[256];
    std::snprintf(key, sizeof(key), "%s\t%u\t%s", service.c_str(), id, name ? name : "?");
    ++g_stats[key];
}

std::map<std::string, ServiceFactory>& Registry() {
    static std::map<std::string, ServiceFactory> registry;
    return registry;
}

} // namespace

void DumpIpcStats() {
    std::lock_guard lock(g_stats_mutex);
    for (const auto& [key, n] : g_stats) {
        std::fprintf(stderr, "ipc\t%s\t%llu\n", key.c_str(), static_cast<unsigned long long>(n));
    }
}

// ---------------------------------------------------------------- Ctx buffers

BufferDescriptor Ctx::InBuffer(size_t i) const {
    if (i < buf_a.size() && buf_a[i].size) return buf_a[i];
    if (i < buf_x.size()) return buf_x[i];
    if (i < buf_a.size()) return buf_a[i];
    return {};
}

BufferDescriptor Ctx::OutBuffer(size_t i) const {
    if (i < buf_b.size() && buf_b[i].size) return buf_b[i];
    if (i < buf_c.size()) return buf_c[i];
    if (i < buf_b.size()) return buf_b[i];
    return {};
}

std::vector<u8> Ctx::ReadBuffer(size_t i) const {
    const BufferDescriptor d = InBuffer(i);
    std::vector<u8> data(d.size);
    if (d.size) g_memory.ReadBlock(d.address, data.data(), d.size);
    return data;
}

std::string Ctx::ReadString(size_t i) const {
    const std::vector<u8> data = ReadBuffer(i);
    const auto end = std::find(data.begin(), data.end(), 0);
    return std::string(data.begin(), end);
}

size_t Ctx::WriteBuffer(const void* data, size_t size, size_t i) const {
    const BufferDescriptor d = OutBuffer(i);
    const size_t n = std::min<size_t>(size, d.size);
    if (n) g_memory.WriteBlock(d.address, data, n);
    return n;
}

// ---------------------------------------------------------------- ServiceObject

void ServiceObject::Register(u32 id, const char* name, Handler handler) {
    commands_[id] = {name, std::move(handler)};
}

void ServiceObject::RegisterStub(u32 id, const char* name) {
    commands_[id] = {name, [](Ctx&) {}};
}

void ServiceObject::Handle(Ctx& ctx) {
    const auto it = commands_.find(ctx.command);
    if (it == commands_.end()) {
        CountCommand(name_, "(unimplemented)", ctx.command);
        if (tts_trace::Enabled()) tts_trace::IpcCount(name_, "(unimplemented)");
        LOG_WARN("%s: unimplemented command %u (answering success with empty output)", name_.c_str(),
                 ctx.command);
        return;
    }
    CountCommand(name_, it->second.name, ctx.command);
    if (tts_trace::Enabled()) tts_trace::IpcCount(name_, it->second.name);
    LOG_DEBUG("%s::%s", name_.c_str(), it->second.name);
    it->second.handler(ctx);
}

std::shared_ptr<KClientSession> MakeSession(std::shared_ptr<ServiceObject> object, bool tipc) {
    auto s = std::make_shared<KClientSession>();
    s->state = std::make_shared<KClientSession::State>();
    s->state->object = std::move(object);
    s->state->tipc = tipc;
    return s;
}

// ---------------------------------------------------------------- shared memory

#ifdef _WIN32

// UNTESTED on Windows (no toolchain here). The host side owns an anonymous
// section (the pagefile-backed equivalent of the memfd) and keeps a view of it
// in host_. `native_handle_` carries the pointer-sized section HANDLE.
KSharedMemory::KSharedMemory(std::string name, u64 size) : name_(std::move(name)), size_(size) {
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        static_cast<DWORD>(size_ >> 32),
                                        static_cast<DWORD>(size_ & 0xffffffffu), nullptr);
    if (!section) Fatal("could not create shared memory %s (error %lu)", name_.c_str(), GetLastError());
    native_handle_ = reinterpret_cast<std::intptr_t>(section);
    void* p = MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, static_cast<SIZE_T>(size_));
    if (!p) Fatal("could not map shared memory %s (error %lu)", name_.c_str(), GetLastError());
    host_ = static_cast<u8*>(p);
}

KSharedMemory::~KSharedMemory() {
    if (host_) UnmapViewOfFile(host_);
    if (native_handle_ != -1) CloseHandle(reinterpret_cast<HANDLE>(native_handle_));
}

u32 MapSharedMemory(u32 handle, u64 address, u64 size, u32 perm) {
    std::shared_ptr<KSharedMemory> shm;
    {
        std::lock_guard lock(g_kernel.lock);
        shm = g_kernel.handles.GetAs<KSharedMemory>(handle);
    }
    if (!shm) return rc::InvalidHandle;
    if (size > shm->Size()) return rc::InvalidSize;
    if (!g_memory.MapFile(address, size, shm->NativeHandle(), 0, MemState::Shared, perm)) {
        return rc::InvalidMemoryRegion;
    }
    OnSharedMemoryMapped(shm->Name(), address, size);
    LOG_DEBUG("mapped shared memory %s at 0x%llx", shm->Name().c_str(), (unsigned long long)address);
    return rc::Success;
}

u32 UnmapSharedMemory(u32, u64 address, u64 size) {
    OnSharedMemoryUnmapped(address, size);
    g_memory.Unmap(address, size);
    return rc::Success;
}

#else

KSharedMemory::KSharedMemory(std::string name, u64 size) : name_(std::move(name)), size_(size) {
    native_handle_ = memfd_create(("nxrt-" + name_).c_str(), MFD_CLOEXEC);
    if (native_handle_ < 0 ||
        ftruncate(static_cast<int>(native_handle_), static_cast<off_t>(size_)) != 0) {
        Fatal("could not create shared memory %s", name_.c_str());
    }
    void* p = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED,
                   static_cast<int>(native_handle_), 0);
    if (p == MAP_FAILED) Fatal("could not map shared memory %s", name_.c_str());
    host_ = static_cast<u8*>(p);
}

KSharedMemory::~KSharedMemory() {
    if (host_) munmap(host_, size_);
    if (native_handle_ >= 0) close(static_cast<int>(native_handle_));
}

u32 MapSharedMemory(u32 handle, u64 address, u64 size, u32 perm) {
    std::shared_ptr<KSharedMemory> shm;
    {
        std::lock_guard lock(g_kernel.lock);
        shm = g_kernel.handles.GetAs<KSharedMemory>(handle);
    }
    if (!shm) return rc::InvalidHandle;
    if (size > shm->Size()) return rc::InvalidSize;
    if (!g_memory.MapFile(address, size, shm->NativeHandle(), 0, MemState::Shared, perm)) {
        return rc::InvalidMemoryRegion;
    }
    OnSharedMemoryMapped(shm->Name(), address, size);
    LOG_DEBUG("mapped shared memory %s at 0x%llx", shm->Name().c_str(), (unsigned long long)address);
    return rc::Success;
}

u32 UnmapSharedMemory(u32, u64 address, u64 size) {
    OnSharedMemoryUnmapped(address, size);
    g_memory.Unmap(address, size);
    return rc::Success;
}

#endif

// ---------------------------------------------------------------- registry and sm:

void RegisterService(const std::string& name, ServiceFactory factory) {
    Registry()[name] = std::move(factory);
}

namespace {

// Answers every command with success and no output. Only for services the
// game opens but this runtime does not know yet; the log names them.
class UnknownService final : public ServiceObject {
public:
    explicit UnknownService(const std::string& name) : ServiceObject(name) {}
};

std::shared_ptr<ServiceObject> OpenService(const std::string& name) {
    const auto it = Registry().find(name);
    if (it == Registry().end()) {
        LOG_WARN("service '%s' is not implemented; using an empty stand-in", name.c_str());
        return std::make_shared<UnknownService>(name);
    }
    return it->second();
}

class ServiceManager final : public ServiceObject {
public:
    ServiceManager() : ServiceObject("sm:") {
        RegisterStub(0, "RegisterClient");
        Register(1, "GetServiceHandle", [](Ctx& ctx) {
            const u64 raw = ctx.Pop<u64>();
            char name[9] = {};
            std::memcpy(name, &raw, 8);
            LOG_INFO("sm: GetServiceHandle(\"%s\")", name);
            ctx.PushMoveObject(MakeSession(OpenService(name)));
        });
    }
};

} // namespace

u32 ConnectToNamedPort(const std::string& name, u32* out_handle) {
    if (name != "sm:") {
        LOG_ERROR("ConnectToNamedPort(\"%s\"): unknown port", name.c_str());
        return rc::NotFound;
    }
    std::lock_guard lock(g_kernel.lock);
    *out_handle = g_kernel.handles.Add(MakeSession(std::make_shared<ServiceManager>(), true));
    return rc::Success;
}

// ---------------------------------------------------------------- message parsing

namespace {

class MessageReader {
public:
    explicit MessageReader(u64 base) : base_(base) {}
    u32 Word() {
        const u32 v = g_memory.Read<u32>(base_ + pos_ * 4);
        ++pos_;
        return v;
    }
    u64 Pos() const { return pos_; }
    void Seek(u64 pos) { pos_ = pos; }

private:
    u64 base_;
    u64 pos_ = 0;
};

BufferDescriptor ParseX(u32 w0, u32 w1, u32* out_counter) {
    BufferDescriptor d;
    d.address = w1 | (static_cast<u64>((w0 >> 12) & 0xF) << 32) | (static_cast<u64>((w0 >> 6) & 0x7) << 36);
    d.size = w0 >> 16;
    *out_counter = (w0 & 0x3F) | (((w0 >> 9) & 0x7) << 9);
    return d;
}

BufferDescriptor ParseAbw(u32 w0, u32 w1, u32 w2) {
    BufferDescriptor d;
    d.size = w0 | (static_cast<u64>((w2 >> 24) & 0xF) << 32);
    d.address = w1 | (static_cast<u64>((w2 >> 28) & 0xF) << 32) | (static_cast<u64>((w2 >> 2) & 0x7) << 36);
    return d;
}

BufferDescriptor ParseC(u32 w0, u32 w1) {
    BufferDescriptor d;
    d.address = w0 | (static_cast<u64>(w1 & 0xFFFF) << 32);
    d.size = w1 >> 16;
    return d;
}

std::shared_ptr<ServiceObject> AddDomainObject(KClientSession::State& st, std::shared_ptr<ServiceObject> o,
                                               u32* out_id) {
    const u32 id = st.next_object_id++;
    st.domain[id] = o;
    *out_id = id;
    return o;
}

// Writes a reply. `domain` adds the domain response header and appends output
// object ids; `tipc` uses the bare TIPC layout.
void WriteReply(u64 msg, Ctx& ctx, bool domain, bool tipc, const std::vector<u32>& out_object_ids) {
    std::vector<u32> handles_copy, handles_move;
    {
        std::lock_guard lock(g_kernel.lock);
        for (auto& o : ctx.copy_out) {
            handles_copy.push_back(g_kernel.handles.Add(o));
            const auto* ev = dynamic_cast<const KEvent*>(o.get());
            LOG_DEBUG("  -> copy handle 0x%x (%s)", handles_copy.back(), ev ? ev->name.c_str() : o->TypeName());
        }
        for (auto& o : ctx.move_out) handles_move.push_back(g_kernel.handles.Add(o));
    }
    std::vector<u32> words;
    words.resize(2);
    const bool has_handles = !handles_copy.empty() || !handles_move.empty();
    if (has_handles) {
        words.push_back((static_cast<u32>(handles_copy.size()) << 1) |
                        (static_cast<u32>(handles_move.size()) << 5));
        for (u32 h : handles_copy) words.push_back(h);
        for (u32 h : handles_move) words.push_back(h);
    }
    std::vector<u8>& out = ctx.Output();
    out.resize((out.size() + 3) & ~size_t{3});
    const size_t data_start = words.size();
    if (tipc) {
        words.push_back(ctx.result);
        for (size_t i = 0; i < out.size(); i += 4) {
            u32 w;
            std::memcpy(&w, out.data() + i, 4);
            words.push_back(w);
        }
    } else {
        // Pad to a 16-byte boundary; the message base is 16-byte aligned.
        while (words.size() % 4) words.push_back(0);
        if (domain) {
            words.push_back(static_cast<u32>(out_object_ids.size()));
            words.push_back(0);
            words.push_back(0);
            words.push_back(0);
        }
        words.push_back(kSfco);
        words.push_back(0);
        words.push_back(ctx.result);
        words.push_back(0);
        for (size_t i = 0; i < out.size(); i += 4) {
            u32 w;
            std::memcpy(&w, out.data() + i, 4);
            words.push_back(w);
        }
        for (u32 id : out_object_ids) words.push_back(id);
    }
    // data_size counts the raw words, alignment padding included.
    const u32 data_size = static_cast<u32>(words.size() - data_start);
    words[0] = 0; // command type is ignored in replies
    words[1] = data_size | (has_handles ? (1u << 31) : 0);
    if (words.size() * 4 > 0x100) {
        LOG_ERROR("reply of %zu bytes does not fit the message buffer", words.size() * 4);
        words.resize(0x40);
    }
    g_memory.WriteBlock(msg, words.data(), words.size() * 4);
}

} // namespace

u32 SendSyncRequest(KThread* thread, u32 handle, u64 msg, u64 msg_size) {
    std::shared_ptr<KClientSession> session;
    {
        std::lock_guard lock(g_kernel.lock);
        session = g_kernel.handles.GetAs<KClientSession>(handle);
    }
    if (!session) {
        LOG_ERROR("SendSyncRequest on invalid handle 0x%x from %s", handle,
                  Symbolize(thread->ctx.x[30]).c_str());
        return rc::InvalidHandle;
    }
    (void)msg_size;
    KClientSession::State& st = *session->state;

    MessageReader r(msg);
    const u32 h0 = r.Word();
    const u32 h1 = r.Word();
    const u32 type = h0 & 0xFFFF;
    const u32 num_x = (h0 >> 16) & 0xF;
    const u32 num_a = (h0 >> 20) & 0xF;
    const u32 num_b = (h0 >> 24) & 0xF;
    const u32 num_w = (h0 >> 28) & 0xF;
    const u32 data_size = h1 & 0x3FF;
    const u32 c_flags = (h1 >> 10) & 0xF;
    const bool has_handle_desc = h1 >> 31;

    Ctx ctx;
    ctx.thread = thread;
    if (has_handle_desc) {
        const u32 hd = r.Word();
        if (hd & 1) {
            r.Word();
            r.Word();
            ctx.pid = 0x51; // the kernel fills in the real process id
        }
        const u32 ncopy = (hd >> 1) & 0xF;
        const u32 nmove = (hd >> 5) & 0xF;
        for (u32 i = 0; i < ncopy; ++i) ctx.copy_handles.push_back(r.Word());
        for (u32 i = 0; i < nmove; ++i) ctx.move_handles.push_back(r.Word());
    }
    for (u32 i = 0; i < num_x; ++i) {
        const u32 w0 = r.Word(), w1 = r.Word();
        u32 counter;
        ctx.buf_x.push_back(ParseX(w0, w1, &counter));
    }
    auto abw = [&](std::vector<BufferDescriptor>& v, u32 n) {
        for (u32 i = 0; i < n; ++i) {
            const u32 w0 = r.Word(), w1 = r.Word(), w2 = r.Word();
            v.push_back(ParseAbw(w0, w1, w2));
        }
    };
    abw(ctx.buf_a, num_a);
    abw(ctx.buf_b, num_b);
    abw(ctx.buf_w, num_w);
    const u64 raw_start = r.Pos();
    // C descriptors follow the raw data.
    if (c_flags >= 2) {
        r.Seek(raw_start + data_size);
        const u32 nc = c_flags == 2 ? 1 : c_flags - 2;
        for (u32 i = 0; i < nc; ++i) {
            const u32 w0 = r.Word(), w1 = r.Word();
            ctx.buf_c.push_back(ParseC(w0, w1));
        }
    }

    // ---- TIPC
    if (type >= kTipcFirst || type == kTipcClose) {
        if (type == kTipcClose) return rc::SessionClosed;
        ctx.command = type - kTipcFirst;
        std::vector<u8> in(data_size * 4);
        g_memory.ReadBlock(msg + raw_start * 4, in.data(), in.size());
        ctx.SetInput(std::move(in));
        st.object->Handle(ctx);
        WriteReply(msg, ctx, false, true, {});
        return rc::Success;
    }

    if (type == kClose) {
        return rc::SessionClosed;
    }

    // ---- CMIF: raw data begins at the next 16-byte boundary.
    u64 pos = (raw_start + 3) & ~u64{3};
    std::shared_ptr<ServiceObject> target = st.object;
    bool domain_message = false;
    if (st.is_domain && (type == kRequest || type == kRequestWithContext)) {
        domain_message = true;
        r.Seek(pos);
        const u32 d0 = r.Word();
        const u32 object_id = r.Word();
        r.Word();
        r.Word();
        const u32 dcmd = d0 & 0xFF;
        const u32 num_in_objects = (d0 >> 8) & 0xFF;
        const u32 payload_size = d0 >> 16;
        // Input object ids sit right after the payload.
        r.Seek(pos + 4 + payload_size / 4);
        for (u32 i = 0; i < num_in_objects; ++i) {
            const u32 id = r.Word();
            const auto it = st.domain.find(id);
            ctx.in_objects.push_back(it == st.domain.end() ? nullptr : it->second);
        }
        if (dcmd == 2) { // CloseVirtualHandle
            st.domain.erase(object_id);
            WriteReply(msg, ctx, true, false, {});
            return rc::Success;
        }
        const auto it = st.domain.find(object_id);
        if (it == st.domain.end()) {
            LOG_ERROR("domain request for unknown object %u", object_id);
            ctx.result = MakeResult(10, 236); // TargetNotFound
            WriteReply(msg, ctx, true, false, {});
            return rc::Success;
        }
        target = it->second;
        pos += 4;
    }

    r.Seek(pos);
    const u32 magic = r.Word();
    r.Word(); // version
    ctx.command = r.Word();
    r.Word(); // token
    if (magic != kSfci) {
        LOG_WARN("%s: request without SFCI magic (type %u)", target->Name().c_str(), type);
    }
    const u64 args_start = pos + 4;
    const u64 raw_end = raw_start + data_size;
    std::vector<u8> in(raw_end > args_start ? (raw_end - args_start) * 4 : 0);
    if (!in.empty()) g_memory.ReadBlock(msg + args_start * 4, in.data(), in.size());
    ctx.SetInput(std::move(in));

    if (type == kControl || type == kControlWithContext) {
        CountCommand("IpcController", "control", ctx.command);
        switch (ctx.command) {
        case 0: { // ConvertCurrentObjectToDomain
            u32 id = 0;
            if (!st.is_domain) {
                st.is_domain = true;
                AddDomainObject(st, st.object, &id);
            } else {
                id = st.domain.begin()->first;
            }
            ctx.Push<u32>(id);
            break;
        }
        case 2:   // CloneCurrentObject
        case 4: { // CloneCurrentObjectEx
            auto clone = std::make_shared<KClientSession>();
            clone->state = session->state;
            ctx.PushMoveObject(clone);
            break;
        }
        case 3: // QueryPointerBufferSize
            ctx.Push<u16>(st.object->PointerBufferSize());
            break;
        default:
            LOG_WARN("unknown control command %u", ctx.command);
            break;
        }
        WriteReply(msg, ctx, false, false, {});
        return rc::Success;
    }

    if (type != kRequest && type != kRequestWithContext) {
        LOG_ERROR("unsupported IPC command type %u", type);
        return rc::NotImplemented;
    }

    target->Handle(ctx);

    std::vector<u32> object_ids;
    if (domain_message) {
        for (auto& o : ctx.objects_out) {
            u32 id;
            AddDomainObject(st, o, &id);
            object_ids.push_back(id);
        }
    } else {
        for (auto& o : ctx.objects_out) ctx.move_out.push_back(MakeSession(o));
    }
    WriteReply(msg, ctx, domain_message, false, object_ids);
    return rc::Success;
}

} // namespace nxrt::hle
