// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>

#include "nvn/recorder.h"
#include "replay/capture_format.h"

namespace nxrt::replay {

struct CaptureOptions {
    u64 first_frame = 0;
    u64 frame_count = 0;
    std::filesystem::path directory;
    std::filesystem::path executable;

    constexpr bool ShouldStartBeforePresent(u64 frame) const noexcept {
        return first_frame != 0 && frame == first_frame - 1;
    }
    constexpr bool ShouldStartOnFirstExecute() const noexcept { return first_frame == 0; }
    constexpr bool IncludesPresent(u64 frame) const noexcept {
        return frame >= first_frame && frame - first_frame < frame_count;
    }
};

// Returns nullopt when capture is not enabled. Invalid opt-in configuration is logged.
std::optional<CaptureOptions> CaptureOptionsFromEnvironment(const std::filesystem::path& executable);

class CapturingRenderer final : public nvn::Renderer {
public:
    CapturingRenderer(nvn::Renderer& renderer, CaptureOptions options);
    ~CapturingRenderer() override;

    void Execute(const nvn::CommandList& list) override;
    void ExecuteSubmission(const nvn::SubmissionSnapshot& submission) override;
    void Present(nvn::GuestAddr texture, u64 frame) override;
    bool InvalidateMappedRange(nvn::GuestAddr pool, u64 offset, u64 size) override {
        std::lock_guard lock(capture_lock_);
        const bool changed = renderer_.InvalidateMappedRange(pool, offset, size);
        if (active_ && changed) Fail("mapped texture readback is not represented in this capture format");
        return changed;
    }

private:
    void ExecuteImpl(const nvn::CommandList& list, const nvn::SubmissionSnapshot* submission);
    void Begin();
    void Finish();
    void Fail(const char* message);

    nvn::Renderer& renderer_;
    CaptureOptions options_;
    // Recorder callbacks can arrive from several guest threads. Serialize capture
    // state, file writes, and forwarding in the same order those callbacks enter.
    std::mutex capture_lock_;
    std::ofstream events_;
    std::unique_ptr<PageStore> pages_;
    bool active_ = false;
    bool finished_ = false;
    u64 recorded_presents_ = 0;
    u64 event_count_ = 0;
    u64 unmapped_pages_skipped_ = 0;
};

} // namespace nxrt::replay
