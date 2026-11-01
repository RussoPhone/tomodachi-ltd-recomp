// SPDX-License-Identifier: GPL-3.0-or-later
// Service IPC. The game talks to system services with HIPC messages written to
// its thread-local region and SendSyncRequest; services here are C++ objects in
// the same process, so a request is parsed, dispatched to a handler and the reply
// written back in place - no second process, no kernel message copy.
//
// Supported: CMIF requests and control messages, domains (virtual object ids),
// TIPC (the newer format sm: uses), copy/move handles, A/B/X/C buffers.
#pragma once

#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "kernel/kernel.h"

namespace nxrt::hle {

class ServiceObject;

struct BufferDescriptor {
    u64 address = 0;
    u64 size = 0;
};

// One request as a handler sees it, and the reply it builds.
class Ctx {
public:
    KThread* thread = nullptr;
    u32 command = 0;
    u64 pid = 0;

    // ---- input
    template <typename T>
    T Pop() {
        in_pos_ = (in_pos_ + alignof(T) - 1) & ~(alignof(T) - 1);
        T v{};
        if (in_pos_ + sizeof(T) <= in_.size()) std::memcpy(&v, in_.data() + in_pos_, sizeof(T));
        in_pos_ += sizeof(T);
        return v;
    }
    template <typename T>
    T PopRaw() { // no alignment, for packed layouts
        T v{};
        if (in_pos_ + sizeof(T) <= in_.size()) std::memcpy(&v, in_.data() + in_pos_, sizeof(T));
        in_pos_ += sizeof(T);
        return v;
    }
    void Skip(size_t bytes) { in_pos_ += bytes; }
    const std::vector<u8>& RawInput() const { return in_; }

    std::vector<u32> copy_handles;
    std::vector<u32> move_handles;
    std::vector<BufferDescriptor> buf_a, buf_b, buf_w, buf_x, buf_c;
    std::vector<std::shared_ptr<ServiceObject>> in_objects;

    // "Auto" buffers: the client may send either a mapped (A/B) or a pointer
    // (X/C) buffer; whichever has a size is the one in use.
    BufferDescriptor InBuffer(size_t i = 0) const;
    BufferDescriptor OutBuffer(size_t i = 0) const;
    std::vector<u8> ReadBuffer(size_t i = 0) const;
    std::string ReadString(size_t i = 0) const; // NUL-terminated contents of in buffer i
    size_t WriteBuffer(const void* data, size_t size, size_t i = 0) const;
    template <typename T>
    size_t WriteBuffer(const std::vector<T>& v, size_t i = 0) const {
        return WriteBuffer(v.data(), v.size() * sizeof(T), i);
    }
    size_t OutBufferSize(size_t i = 0) const { return OutBuffer(i).size; }

    // ---- output
    u32 result = rc::Success;
    template <typename T>
    void Push(const T& v) {
        out_.resize((out_.size() + alignof(T) - 1) & ~(alignof(T) - 1));
        const size_t at = out_.size();
        out_.resize(at + sizeof(T));
        std::memcpy(out_.data() + at, &v, sizeof(T));
    }
    void PushRaw(const void* data, size_t size) {
        const size_t at = out_.size();
        out_.resize(at + size);
        std::memcpy(out_.data() + at, data, size);
    }
    // Returns a new handle for `object` to the client (a copy shares it).
    void PushCopyObject(std::shared_ptr<KObject> object) { copy_out.push_back(std::move(object)); }
    void PushMoveObject(std::shared_ptr<KObject> object) { move_out.push_back(std::move(object)); }
    // Returns a sub-interface: a domain object id, or a new session handle.
    void PushInterface(std::shared_ptr<ServiceObject> object) { objects_out.push_back(std::move(object)); }

    std::vector<std::shared_ptr<KObject>> copy_out, move_out;
    std::vector<std::shared_ptr<ServiceObject>> objects_out;
    std::vector<u8>& Output() { return out_; }

    void SetInput(std::vector<u8> in) {
        in_ = std::move(in);
        in_pos_ = 0;
    }

private:
    std::vector<u8> in_;
    size_t in_pos_ = 0;
    std::vector<u8> out_;
};

// A service interface: a table of commands. Sub-interfaces returned by commands
// are ServiceObjects too.
class ServiceObject : public std::enable_shared_from_this<ServiceObject> {
public:
    explicit ServiceObject(std::string name) : name_(std::move(name)) {}
    virtual ~ServiceObject() = default;
    const std::string& Name() const { return name_; }

    using Handler = std::function<void(Ctx&)>;
    void Handle(Ctx& ctx);
    // Pointer buffer size reported to QueryPointerBufferSize.
    virtual u16 PointerBufferSize() const { return 0x8000; }

protected:
    void Register(u32 id, const char* name, Handler handler);
    // Commands that only need to answer success.
    void RegisterStub(u32 id, const char* name);

private:
    struct Command {
        const char* name;
        Handler handler;
    };
    std::string name_;
    std::map<u32, Command> commands_;
};

// A client session handle: the game's end of a connection to a ServiceObject.
// Clones share the state, so a domain stays one domain however it is reached.
class KClientSession final : public KObject {
public:
    struct State {
        bool is_domain = false;
        std::shared_ptr<ServiceObject> object;
        std::map<u32, std::shared_ptr<ServiceObject>> domain; // virtual object ids
        u32 next_object_id = 1;
        bool tipc = false;
    };
    const char* TypeName() const override { return "KClientSession"; }
    std::shared_ptr<State> state;
};

std::shared_ptr<KClientSession> MakeSession(std::shared_ptr<ServiceObject> object, bool tipc = false);

// Kernel object for shared memory: a memfd the host side maps too, so a service
// writes input or time data and the guest sees it.
class KSharedMemory final : public KObject {
public:
    KSharedMemory(std::string name, u64 size);
    ~KSharedMemory() override;
    const char* TypeName() const override { return "KSharedMemory"; }
    u8* HostPointer() const { return host_; }
    u64 Size() const { return size_; }
    std::intptr_t NativeHandle() const { return native_handle_; }
    const std::string& Name() const { return name_; }

private:
    std::string name_;
    u64 size_;
    std::intptr_t native_handle_ = -1;
    u8* host_ = nullptr;
};

// The service registry ("sm:").
using ServiceFactory = std::function<std::shared_ptr<ServiceObject>()>;
void RegisterService(const std::string& name, ServiceFactory factory);
void RegisterAllServices();

u32 ConnectToNamedPort(const std::string& name, u32* out_handle);
u32 SendSyncRequest(KThread* thread, u32 handle, u64 message, u64 size);
u32 MapSharedMemory(u32 handle, u64 address, u64 size, u32 perm);
u32 UnmapSharedMemory(u32 handle, u64 address, u64 size);

// Inventory of commands seen, written at exit when NXRT_IPC_STATS is set.
void DumpIpcStats();

} // namespace nxrt::hle
