// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nvn/model.h"

namespace nxrt::replay {

inline constexpr std::size_t kCapturePageSize = 4096;
inline constexpr u64 kCaptureMaxPagesBytes = 10ULL * 1024 * 1024 * 1024;

void WriteCommand(std::ostream& out, const nvn::Command& command);
nvn::Command ReadCommand(std::istream& in);
void WriteCommandList(std::ostream& out, const nvn::CommandList& list);
nvn::CommandList ReadCommandList(std::istream& in);
std::vector<u64> SubmissionDescriptorPoolObjects(nvn::Model& model, const nvn::CommandList& submitted);
std::vector<u64> ExecutionCpuReadPages(nvn::Model& model, const nvn::CommandList& submitted);
std::vector<u64> DescriptorPoolObjectPages(std::span<const u64> objects);
std::vector<u64> DriverTablePages(u64 base, u64 format_offset, u64 storage_offset, u64 format_table);
std::vector<u64> MemoryPoolObjectPages(std::span<const nvn::GuestAddr> pool_handles);

// The mutex is process-local synchronization. This function locks the model while
// taking the logical snapshot; ReadModel creates a fresh, unlocked mutex.
void WriteModel(std::ostream& out, nvn::Model& model);
std::unique_ptr<nvn::Model> ReadModel(std::istream& in);
void ReadModel(std::istream& in, nvn::Model& destination);
std::string SnapshotExecutionModel(nvn::Model& model, const nvn::CommandList& submitted);

struct PageRef {
    u64 va = 0;
    u64 id = 0;
    friend bool operator==(const PageRef&, const PageRef&) = default;
};

struct PageRefHash {
    std::size_t operator()(const PageRef& ref) const noexcept {
        const auto a=std::hash<u64>{}(ref.va), b=std::hash<u64>{}(ref.id);
        return a^(b+0x9e3779b97f4a7c15ULL+(a<<6)+(a>>2));
    }
};

struct CaptureEvent {
    enum class Type : u8 { Execute = 1, Present = 2, State = 3 };
    Type type = Type::Execute;
    nvn::CommandList commands;
    std::vector<PageRef> pages;
    nvn::GuestAddr texture = 0;
    u64 frame = 0;
    std::string model_snapshot;
    u64 driver_base = 0;
};

void WriteEventsHeader(std::ostream& out);
u32 ReadEventsHeader(std::istream& in);
void WriteEvent(std::ostream& out, const CaptureEvent& event);
CaptureEvent ReadEvent(std::istream& in, u32 version = 3);

class PageStore {
public:
    explicit PageStore(const std::filesystem::path& directory);
    ~PageStore();
    PageStore(const PageStore&) = delete;
    PageStore& operator=(const PageStore&) = delete;

    u64 Put(u64 va, std::span<const std::byte> bytes);
    std::vector<std::byte> Get(u64 id);
    void Close();
    u64 unique_pages() const noexcept { return unique_pages_; }
    const std::vector<PageRef>& references() const noexcept { return references_; }

private:
    std::filesystem::path directory_;
    std::fstream pages_;
    std::ofstream index_;
    std::vector<std::array<std::byte, 32>> hashes_;
    std::vector<u64> offsets_;
    std::vector<PageRef> references_;
    std::unordered_set<PageRef, PageRefHash> reference_set_;
    std::unordered_map<std::string, u64> hash_ids_;
    u64 unique_pages_ = 0;
    bool closed_ = false;
};

class CapturePageReader {
public:
    explicit CapturePageReader(const std::filesystem::path& directory);
    CapturePageReader(const CapturePageReader&) = delete;
    CapturePageReader& operator=(const CapturePageReader&) = delete;

    // The returned view remains valid until the next Read call.
    std::span<const std::byte> Read(u64 id);
    const std::vector<PageRef>& references() const noexcept { return references_; }

private:
    std::ifstream pages_;
    std::vector<u64> offsets_;
    std::vector<std::array<std::byte, 32>> hashes_;
    std::vector<PageRef> references_;
    std::vector<std::byte> resident_pages_;
    std::array<std::byte, kCapturePageSize> scratch_{};
};

struct CaptureManifest {
    u32 version = 3;
    u64 first_frame = 0;
    u64 frame_count = 0;
    std::string date;
    std::string executable_sha256;
};

std::string Sha256File(const std::filesystem::path& file);
void WriteManifest(const std::filesystem::path& file, const CaptureManifest& manifest);

} // namespace nxrt::replay
