// SPDX-License-Identifier: GPL-3.0-or-later
// fsp-srv: the game's RomFS comes from the installed romfs.bin, read in place;
// save data lives in plain folders under the user directory, one per save.
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>

#include "memory.h"
#include "runtime.h"
#include "services/common.h"
#include "services/fs_write_options.h"
#include "tts_trace.h"

namespace nxrt::hle {

namespace fs = std::filesystem;

namespace {

// One open host file with positional I/O. POSIX: open/pread/pwrite. Windows:
// CreateFileW (share read/write/delete, so a save can be removed while open)
// and ReadFile/WriteFile with an explicit offset.
class HostFile {
public:
    HostFile() = default;
    HostFile(const HostFile&) = delete;
    HostFile& operator=(const HostFile&) = delete;
    ~HostFile() { Close(); }

#ifdef _WIN32
    bool Open(const fs::path& path, bool write) {
        h_ = CreateFileW(path.c_str(), write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        return Valid();
    }
    bool Valid() const { return h_ != INVALID_HANDLE_VALUE; }
    s64 Size() const {
        LARGE_INTEGER n;
        return GetFileSizeEx(h_, &n) ? static_cast<s64>(n.QuadPart) : -1;
    }
    s64 Pread(void* buf, u64 n, s64 offset) const {
        OVERLAPPED o{};
        o.Offset = static_cast<DWORD>(offset);
        o.OffsetHigh = static_cast<DWORD>(static_cast<u64>(offset) >> 32);
        DWORD got = 0;
        if (!ReadFile(h_, buf, static_cast<DWORD>(n), &got, &o) && GetLastError() != ERROR_HANDLE_EOF) return -1;
        return got;
    }
    s64 Pwrite(const void* buf, u64 n, s64 offset) const {
        OVERLAPPED o{};
        o.Offset = static_cast<DWORD>(offset);
        o.OffsetHigh = static_cast<DWORD>(static_cast<u64>(offset) >> 32);
        DWORD put = 0;
        return WriteFile(h_, buf, static_cast<DWORD>(n), &put, &o) ? static_cast<s64>(put) : -1;
    }
    bool Flush() const { return FlushFileBuffers(h_) != 0; }
    bool Truncate(s64 size) const {
        LARGE_INTEGER p;
        p.QuadPart = size;
        return SetFilePointerEx(h_, p, nullptr, FILE_BEGIN) && SetEndOfFile(h_);
    }
    void Close() {
        if (Valid()) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    bool Open(const fs::path& path, bool write) {
        fd_ = ::open(path.c_str(), (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
        return Valid();
    }
    bool Valid() const { return fd_ >= 0; }
    s64 Size() const { return static_cast<s64>(lseek(fd_, 0, SEEK_END)); }
    s64 Pread(void* buf, u64 n, s64 offset) const { return pread(fd_, buf, n, offset); }
    s64 Pwrite(const void* buf, u64 n, s64 offset) const { return pwrite(fd_, buf, n, offset); }
    bool Flush() const { return fsync(fd_) == 0; }
    bool Truncate(s64 size) const { return ftruncate(fd_, size) == 0; }
    void Close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
#endif
};

// A path for logs and fixed-size name fields: UTF-8 on every platform.
std::string HostString(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool TraceSaveIoEnabled() {
    static const bool enabled = std::getenv("NXRT_TRACE_SAVE_IO") != nullptr;
    return enabled;
}

u32 NextSaveIoTraceSequence() {
    static std::atomic<u32> emitted{};
    constexpr u32 kMaxTraceLines = 4096;
    u32 sequence = emitted.load(std::memory_order_relaxed);
    while (sequence < kMaxTraceLines &&
           !emitted.compare_exchange_weak(sequence, sequence + 1, std::memory_order_relaxed)) {
    }
    if (sequence >= kMaxTraceLines) return 0;
    return sequence + 1;
}

void TraceSaveIoCapReached(u32 sequence) {
    if (sequence == 4096) LOG_INFO("save-io trace capped at %u events", sequence);
}

std::string SaveRelativePath(const fs::path& root, const fs::path& path) {
    const fs::path relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return "<unresolved>";
    std::string value = HostString(relative.lexically_normal());
    for (char& c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7f) c = '?';
    }
    constexpr size_t kMaxPathBytes = 256;
    if (value.size() > kMaxPathBytes) value.resize(kMaxPathBytes);
    return value.empty() ? "." : value;
}

void TraceSaveIo(const char* operation, const std::string& relative_path,
                 std::optional<s64> offset, std::optional<s64> requested,
                 std::optional<u64> written, u32 result, int host_result = -1,
                 const char* detail = nullptr) {
    if (!TraceSaveIoEnabled()) return;
    const u32 sequence = NextSaveIoTraceSequence();
    if (!sequence) return;
    const auto number = [](const auto& value) {
        return value ? std::to_string(*value) : std::string("-");
    };
    if (detail && *detail) {
        LOG_INFO("save-io #%u op=%s path=%s offset=%s requested=%s written=%s result=0x%08x host_result=%d %s",
                 sequence, operation, relative_path.c_str(), number(offset).c_str(), number(requested).c_str(),
                 number(written).c_str(), result, host_result, detail);
    } else {
        LOG_INFO("save-io #%u op=%s path=%s offset=%s requested=%s written=%s result=0x%08x host_result=%d",
                 sequence, operation, relative_path.c_str(), number(offset).c_str(), number(requested).c_str(),
                 number(written).c_str(), result, host_result);
    }
    TraceSaveIoCapReached(sequence);
}

void TraceSaveIoPath(const char* operation, const fs::path& root, const fs::path& path,
                     std::optional<s64> offset, std::optional<s64> requested,
                     std::optional<u64> written, u32 result, int host_result = -1) {
    if (!TraceSaveIoEnabled()) return;
    TraceSaveIo(operation, SaveRelativePath(root, path), offset, requested, written, result, host_result);
}

void TraceSaveIoOpen(const std::string& relative_path, u32 mode, u32 result, int host_result) {
    if (!TraceSaveIoEnabled()) return;
    char detail[64];
    std::snprintf(detail, sizeof(detail), "mode=0x%08x writable=%u", mode, (mode & 2) ? 1u : 0u);
    TraceSaveIo("open", relative_path, {}, {}, {}, result, host_result, detail);
}

void TraceSaveIoOpenPath(const fs::path& root, const fs::path& path, u32 mode,
                         u32 result, int host_result = -1) {
    if (!TraceSaveIoEnabled()) return;
    TraceSaveIoOpen(SaveRelativePath(root, path), mode, result, host_result);
}

void TraceSaveIoRead(const std::string& relative_path, s64 offset, s64 requested,
                     s64 received, u32 result, int host_result) {
    if (!TraceSaveIoEnabled()) return;
    const u32 sequence = NextSaveIoTraceSequence();
    if (!sequence) return;
    LOG_INFO("save-io #%u op=read path=%s offset=%lld requested=%lld received=%lld result=0x%08x host_result=%d",
             sequence, relative_path.c_str(), static_cast<long long>(offset),
             static_cast<long long>(requested), static_cast<long long>(received), result, host_result);
    TraceSaveIoCapReached(sequence);
}

void TraceSaveIoReadSummary(const std::string& relative_path, u64 calls, u64 requested,
                            u64 received, u64 failures) {
    if (!TraceSaveIoEnabled()) return;
    const u32 sequence = NextSaveIoTraceSequence();
    if (!sequence) return;
    LOG_INFO("save-io #%u op=read-summary path=%s calls=%llu requested_total=%llu received_total=%llu failures=%llu",
             sequence, relative_path.c_str(), static_cast<unsigned long long>(calls),
             static_cast<unsigned long long>(requested), static_cast<unsigned long long>(received),
             static_cast<unsigned long long>(failures));
    TraceSaveIoCapReached(sequence);
}

constexpr u32 kPathNotFound = MakeResult(2, 1);
constexpr u32 kPathAlreadyExists = MakeResult(2, 2);
constexpr u32 kTargetLocked = MakeResult(2, 7);
constexpr u32 kOutOfRange = MakeResult(2, 3005);

// A read-only file served with pread.
class IStorageFile final : public ServiceObject {
public:
    explicit IStorageFile(const fs::path& path) : ServiceObject("IStorage(fs)") {
        if (!file_.Open(path, false)) LOG_ERROR("could not open %s", HostString(path).c_str());
        size_ = file_.Valid() ? file_.Size() : 0;
        if (tts_trace::Enabled() && file_.Valid()) {
            // NXRT_TRACE_TTS: name files by RomFS offset (read-only; the image is parsed once).
            romfs_index_ = std::make_unique<tts_trace::RomfsIndex>();
            const bool ok = romfs_index_->Build(
                    [this](u64 off, void* buf, u64 n) { return file_.Pread(buf, n, static_cast<s64>(off)) == static_cast<s64>(n); });
            if (!ok) romfs_index_.reset();
            tts_trace::FsOpen("romfs.bin (" + HostString(path.filename()) + ", " + std::to_string(size_) + " bytes, " +
                              (ok ? std::to_string(romfs_index_->size()) + " arquivos indexados)" : "tabela RomFS nao lida)"));
        }
        Register(0, "Read", [this](Ctx& ctx) {
            const s64 offset = ctx.Pop<s64>();
            const s64 length = ctx.Pop<s64>();
            const BufferDescriptor out = ctx.OutBuffer();
            if (offset < 0 || length < 0 || offset > size_) {
                ctx.result = kOutOfRange;
                return;
            }
            const u64 n = std::min<u64>(std::min<u64>(static_cast<u64>(length), out.size), static_cast<u64>(size_ - offset));
            const bool trace = tts_trace::Enabled();
            const u64 t0 = trace ? tts_trace::NowNs() : 0;
            if (n && file_.Pread(g_memory.Ptr(out.address, n), n, offset) < 0) ctx.result = kOutOfRange;
            if (trace) TraceRomfsRead(offset, n, tts_trace::NowNs() - t0);
        });
        Register(4, "GetSize", [this](Ctx& ctx) { ctx.Push<s64>(size_); });
    }

private:
    void TraceRomfsRead(s64 offset, u64 n, u64 ns) {
        std::string key = "romfs/<cabecalho/tabelas>";
        s64 shown = offset;
        if (romfs_index_) {
            u64 in_file = 0;
            const std::string& name = romfs_index_->Lookup(static_cast<u64>(offset), &in_file);
            if (!name.empty()) {
                key = "romfs/" + name;
                shown = static_cast<s64>(in_file); // offset inside the file
            }
        } else {
            key = "romfs/<sem indice>";
        }
        tts_trace::FsRead(key, shown, n, ns);
    }

    HostFile file_;
    s64 size_ = 0;
    std::unique_ptr<tts_trace::RomfsIndex> romfs_index_;
};

// A path as the SDK sends it: a NUL-terminated string in a pointer buffer.
// Resolves it under `root` and refuses anything that climbs out.
bool ResolvePath(const fs::path& root, Ctx& ctx, fs::path& out) {
    std::string p = ctx.ReadString(0);
    while (!p.empty() && p.front() == '/') p.erase(0, 1);
    const fs::path rel = fs::path(p).lexically_normal();
    if (!rel.empty() && (*rel.begin() == "..")) return false;
    out = rel.empty() || rel == "." ? root : root / rel;
    return true;
}

class IFile final : public ServiceObject {
public:
    IFile(const fs::path& path, std::string relative_path, u32 mode)
        : ServiceObject("IFile"), relative_path_(std::move(relative_path)) {
        const bool opened = file_.Open(path, (mode & 2) != 0);
        TraceSaveIoOpen(relative_path_, mode, rc::Success, opened ? 0 : 1);
        if (tts_trace::Enabled())
            tts_trace::FsOpen("save/" + relative_path_ + " mode=" + std::to_string(mode) + (opened ? "" : " FALHOU"));
        Register(0, "Read", [this](Ctx& ctx) {
            ctx.Pop<u32>(); // option
            const s64 offset = ctx.Pop<s64>();
            const s64 size = ctx.Pop<s64>();
            const BufferDescriptor out = ctx.OutBuffer();
            const u64 n = std::min<u64>(static_cast<u64>(size), out.size);
            const bool trace = tts_trace::Enabled();
            const u64 t0 = trace ? tts_trace::NowNs() : 0;
            const s64 got = n ? file_.Pread(g_memory.Ptr(out.address, n), n, offset) : 0;
            if (trace) tts_trace::FsRead("save/" + relative_path_, offset, got > 0 ? static_cast<u64>(got) : 0,
                                         tts_trace::NowNs() - t0);
            RecordRead(offset, size, got, ctx.result);
            ctx.Push<s64>(got < 0 ? 0 : got);
        });
        Register(1, "Write", [this](Ctx& ctx) {
            const u32 option = ctx.Pop<u32>();
            const s64 offset = ctx.Pop<s64>();
            const s64 size = ctx.Pop<s64>();
            const BufferDescriptor in = ctx.InBuffer();
            if (offset < 0 || size < 0) {
                ctx.result = kOutOfRange;
                TraceSaveIo("write", relative_path_, offset, size, 0, ctx.result);
                return;
            }
            const u64 n = std::min<u64>(static_cast<u64>(size), in.size);
            u64 written = 0;
            const bool ok = detail::WriteFileWithOptions(
                    option, g_memory.Ptr(in.address, n), n, offset,
                    [this, &written](const u8* data, u64 length, s64 at) {
                        const s64 count = file_.Pwrite(data, length, at);
                        if (count > 0 && static_cast<u64>(count) <= length) written += static_cast<u64>(count);
                        return count;
                    },
                    [this] {
                        const bool flushed = file_.Flush();
                        TraceSaveIo("flush", relative_path_, {}, {}, {},
                                    flushed ? rc::Success : kTargetLocked, flushed ? 0 : 1);
                        return flushed;
                    });
            if (!ok)
                ctx.result = kTargetLocked;
            TraceSaveIo("write", relative_path_, offset, size, written, ctx.result);
        });
        Register(2, "Flush", [this](Ctx& ctx) {
            const bool flushed = file_.Flush();
            if (!flushed) ctx.result = kTargetLocked;
            TraceSaveIo("flush", relative_path_, {}, {}, {}, ctx.result, flushed ? 0 : 1);
        });
        Register(3, "SetSize", [this](Ctx& ctx) {
            if (!file_.Truncate(ctx.Pop<s64>())) ctx.result = kTargetLocked;
        });
        Register(4, "GetSize", [this](Ctx& ctx) { ctx.Push<s64>(file_.Size()); });
    }
    ~IFile() override {
        if (!TraceSaveIoEnabled()) return;
        std::lock_guard<std::mutex> lock(read_mutex_);
        EmitReadSummary(read_calls_);
    }

    bool Ok() const { return file_.Valid(); }

private:
    static bool IsReadSummaryCheckpoint(u64 calls) {
        return calls == 64 || calls == 256 || calls == 1024 || calls == 4096 ||
               calls == 16384 || calls == 65536 || calls == 262144 || calls == 1048576;
    }

    void RecordRead(s64 offset, s64 requested, s64 received, u32 result) {
        if (!TraceSaveIoEnabled()) return;
        std::lock_guard<std::mutex> lock(read_mutex_);
        const u64 safe_requested = requested > 0 ? static_cast<u64>(requested) : 0;
        const u64 safe_received = received > 0 ? static_cast<u64>(received) : 0;
        read_requested_ += safe_requested;
        read_received_ += safe_received;
        if (received < 0) ++read_failures_;
        const u64 calls = ++read_calls_;
        if (calls <= kDetailedReadEvents) {
            TraceSaveIoRead(relative_path_, offset, requested, received, result, received < 0 ? 1 : 0);
        } else if (IsReadSummaryCheckpoint(calls)) {
            EmitReadSummary(calls);
        }
    }

    void EmitReadSummary(u64 calls) {
        if (!TraceSaveIoEnabled() || calls <= kDetailedReadEvents) return;
        if (calls <= read_summary_calls_) return;
        read_summary_calls_ = calls;
        TraceSaveIoReadSummary(relative_path_, calls, read_requested_, read_received_, read_failures_);
    }

    static constexpr u64 kDetailedReadEvents = 8;
    HostFile file_;
    std::string relative_path_;
    std::mutex read_mutex_;
    u64 read_calls_ = 0;
    u64 read_requested_ = 0;
    u64 read_received_ = 0;
    u64 read_failures_ = 0;
    u64 read_summary_calls_ = 0;
};

struct DirectoryEntry {
    char name[0x301];
    u8 pad[3];
    s8 type; // 0 directory, 1 file
    u8 pad2[3];
    s64 size;
};
static_assert(sizeof(DirectoryEntry) == 0x310);

class IDirectory final : public ServiceObject {
public:
    IDirectory(const fs::path& path, u32 mode) : ServiceObject("IDirectory") {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(path, ec)) {
            const bool is_dir = e.is_directory();
            if ((is_dir && !(mode & 1)) || (!is_dir && !(mode & 2))) continue;
            DirectoryEntry d{};
            std::strncpy(d.name, HostString(e.path().filename()).c_str(), sizeof(d.name) - 1);
            d.type = is_dir ? 0 : 1;
            d.size = is_dir ? 0 : static_cast<s64>(e.file_size(ec));
            entries_.push_back(d);
        }
        Register(0, "Read", [this](Ctx& ctx) {
            const size_t room = ctx.OutBufferSize() / sizeof(DirectoryEntry);
            const size_t n = std::min(room, entries_.size() - next_);
            ctx.WriteBuffer(entries_.data() + next_, n * sizeof(DirectoryEntry));
            next_ += n;
            ctx.Push<s64>(n);
        });
        Register(1, "GetEntryCount", [this](Ctx& ctx) { ctx.Push<s64>(entries_.size() - next_); });
    }

private:
    std::vector<DirectoryEntry> entries_;
    size_t next_ = 0;
};

class IFileSystem final : public ServiceObject {
public:
    explicit IFileSystem(fs::path root) : ServiceObject("IFileSystem"), root_(std::move(root)) {
        std::error_code ec;
        fs::create_directories(root_, ec);
        Register(0, "CreateFile", [this](Ctx& ctx) {
            ctx.Pop<s32>(); // option
            const s64 size = ctx.Pop<s64>();
            fs::path p;
            if (!Path(ctx, p)) {
                TraceSaveIo("create", "<invalid>", {}, size, {}, ctx.result);
                return;
            }
            if (fs::exists(p)) {
                ctx.result = kPathAlreadyExists;
                TraceSaveIoPath("create", root_, p, {}, size, {}, ctx.result);
                return;
            }
            std::ofstream created(p, std::ios::binary);
            created.close();
            const int create_result = created.fail() ? 1 : 0;
            std::error_code ec;
            fs::resize_file(p, static_cast<u64>(size), ec);
            TraceSaveIoPath("create", root_, p, {}, size, 0, ctx.result, ec ? ec.value() : create_result);
        });
        Register(1, "DeleteFile", [this](Ctx& ctx) {
            fs::path p;
            if (!Path(ctx, p)) return;
            std::error_code ec;
            if (!fs::remove(p, ec)) ctx.result = kPathNotFound;
        });
        Register(2, "CreateDirectory", [this](Ctx& ctx) {
            fs::path p;
            if (!Path(ctx, p)) return;
            if (fs::exists(p)) ctx.result = kPathAlreadyExists;
            else fs::create_directory(p);
        });
        Register(3, "DeleteDirectory", [this](Ctx& ctx) {
            fs::path p;
            if (!Path(ctx, p)) return;
            std::error_code ec;
            if (!fs::remove(p, ec)) ctx.result = kPathNotFound;
        });
        Register(4, "DeleteDirectoryRecursively", [this](Ctx& ctx) {
            fs::path p;
            if (!Path(ctx, p)) return;
            std::error_code ec;
            if (!fs::remove_all(p, ec)) ctx.result = kPathNotFound;
        });
        Register(5, "RenameFile", [this](Ctx& ctx) { Rename(ctx); });
        Register(6, "RenameDirectory", [this](Ctx& ctx) { Rename(ctx); });
        Register(7, "GetEntryType", [this](Ctx& ctx) {
            fs::path p;
            if (!Path(ctx, p)) return;
            if (!fs::exists(p)) {
                ctx.result = kPathNotFound;
                return;
            }
            ctx.Push<u32>(fs::is_directory(p) ? 0 : 1);
        });
        Register(8, "OpenFile", [this](Ctx& ctx) {
            const u32 mode = ctx.Pop<u32>();
            fs::path p;
            if (!Path(ctx, p)) {
                TraceSaveIoOpen("<invalid>", mode, ctx.result, -1);
                return;
            }
            if (!fs::is_regular_file(p)) {
                ctx.result = kPathNotFound;
                TraceSaveIoOpenPath(root_, p, mode, ctx.result);
                return;
            }
            std::string relative_path;
            if (TraceSaveIoEnabled() || tts_trace::Enabled()) relative_path = SaveRelativePath(root_, p);
            ctx.PushInterface(std::make_shared<IFile>(p, std::move(relative_path), mode));
        });
        Register(9, "OpenDirectory", [this](Ctx& ctx) {
            const u32 mode = ctx.Pop<u32>();
            fs::path p;
            if (!Path(ctx, p)) return;
            if (!fs::is_directory(p)) {
                ctx.result = kPathNotFound;
                return;
            }
            ctx.PushInterface(std::make_shared<IDirectory>(p, mode));
        });
        Register(10, "Commit", [](Ctx&) {
            TraceSaveIo("commit", ".", {}, {}, {}, rc::Success);
        });
        Register(11, "GetFreeSpaceSize", [](Ctx& ctx) { ctx.Push<s64>(0x40000000); });
        Register(12, "GetTotalSpaceSize", [](Ctx& ctx) { ctx.Push<s64>(0x80000000); });
        Register(13, "CleanDirectoryRecursively", [this](Ctx& ctx) {
            fs::path p;
            if (!Path(ctx, p)) return;
            std::error_code ec;
            for (const auto& e : fs::directory_iterator(p, ec)) fs::remove_all(e.path(), ec);
        });
    }

private:
    bool Path(Ctx& ctx, fs::path& out) {
        if (ResolvePath(root_, ctx, out)) return true;
        ctx.result = kPathNotFound;
        return false;
    }
    void Rename(Ctx& ctx) {
        fs::path from, to;
        std::string b = ctx.ReadString(1);
        if (!ResolvePath(root_, ctx, from)) return;
        while (!b.empty() && b.front() == '/') b.erase(0, 1);
        to = root_ / fs::path(b).lexically_normal();
        std::error_code ec;
        fs::rename(from, to, ec);
        if (ec) ctx.result = kPathNotFound;
    }

    fs::path root_;
};

struct SaveDataAttribute {
    u64 program_id;
    u64 user_id[2];
    u64 system_save_data_id;
    u8 type;
    u8 rank;
    u16 index;
    u32 pad;
    u64 reserved[3];
};
static_assert(sizeof(SaveDataAttribute) == 0x40);

fs::path SaveRoot(u8 space, const SaveDataAttribute& a) {
    char name[96];
    std::snprintf(name, sizeof(name), "%u/%016llx/%016llx%016llx/%u", space,
                  static_cast<unsigned long long>(a.program_id ? a.program_id : 0x010051F0207B2000ULL),
                  static_cast<unsigned long long>(a.user_id[1]), static_cast<unsigned long long>(a.user_id[0]),
                  a.type);
    return g_config.user_dir / "save" / name;
}

class FspSrv final : public ServiceObject {
public:
    FspSrv() : ServiceObject("fsp-srv") {
        RegisterStub(1, "SetCurrentProcess");
        Register(51, "OpenSaveDataFileSystem", [](Ctx& ctx) {
            const u8 space = ctx.Pop<u8>();
            const SaveDataAttribute a = ctx.Pop<SaveDataAttribute>();
            const fs::path root = SaveRoot(space, a);
            // Save roots live under the user's host directory; do not expose that path in logs.
            LOG_INFO("save data (space %u, type %u)", space, a.type);
            ctx.PushInterface(std::make_shared<IFileSystem>(root));
        });
        Register(53, "OpenReadOnlySaveDataFileSystem", [](Ctx& ctx) {
            const u8 space = ctx.Pop<u8>();
            const SaveDataAttribute a = ctx.Pop<SaveDataAttribute>();
            ctx.PushInterface(std::make_shared<IFileSystem>(SaveRoot(space, a)));
        });
        Register(200, "OpenDataStorageByCurrentProcess", [](Ctx& ctx) {
            ctx.PushInterface(std::make_shared<IStorageFile>(g_config.game_dir / "exefs" / "romfs.bin"));
        });
        Register(203, "OpenPatchDataStorageByCurrentProcess", [](Ctx& ctx) { ctx.result = MakeResult(2, 1001); });
        Register(631, "IsSdCardAccessible", [](Ctx& ctx) { ctx.Push<u8>(0); });
        RegisterStub(1003, "DisableAutoSaveDataCreation");
        RegisterStub(1004, "SetGlobalAccessLogMode");
        Register(1005, "GetGlobalAccessLogMode", [](Ctx& ctx) { ctx.Push<u32>(0); });
        RegisterStub(1006, "OutputAccessLogToSdCard");
        Register(1011, "GetProgramIndexForAccessLog", [](Ctx& ctx) {
            ctx.Push<u32>(0); // access log version
            ctx.Push<u32>(0); // program index
        });
        RegisterStub(1016, "FlushAccessLogOnSdCard");
    }
};

} // namespace

void RegisterFsServices() {
    RegisterService("fsp-srv", [] { return std::make_shared<FspSrv>(); });
}

} // namespace nxrt::hle
