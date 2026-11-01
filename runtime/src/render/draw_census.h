// Opt-in draw census (NXRT_DRAW_CENSUS=<file.csv>): per (frame, program) counts of the draws the game requested,
// the draws the renderer executed and the draws it dropped, split by drop reason. Header-only so the aggregator is
// unit-testable without Vulkan. Not thread-safe: the renderer calls it from its single Execute thread.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nxrt::render {

// Drop reasons: every `skip(...)` string of NvnRenderer::Draw maps to one of these (prefix match; unknown = Other).
enum class DropReason : unsigned {
    RenderDisabled, StreamFlushFailed, NoTargets, Target, Quads, NoProgram, PipelineFailed, StreamFull,
    BufferOrImageDescriptors, DepthSampleDump, FloorD32Mirror, FeedbackSnapshot, ImageView, VertexBuffer,
    VertexUnmapped, IndexBuffer, IndexUnmapped, PushDescriptorProc, DescriptorPool, Other, Count
};

inline constexpr std::array<const char*, static_cast<size_t>(DropReason::Count)> kDropReasonNames = {
    "render_disabled", "stream_flush_failed", "no_targets", "target", "quads", "no_program", "pipeline_failed",
    "stream_full", "buffer_or_image_descriptors", "depth_sample_dump", "floor_d32_mirror", "feedback_snapshot",
    "image_view", "vertex_buffer", "vertex_unmapped", "index_buffer", "index_unmapped", "push_descriptor_proc",
    "descriptor_pool", "other"};

inline DropReason ClassifyDropReason(std::string_view why) {
    const auto is = [&](std::string_view p) { return why.substr(0, p.size()) == p; };
    if (is("render-disabled")) return DropReason::RenderDisabled;
    if (is("stream-flush-failed")) return DropReason::StreamFlushFailed;
    if (is("no-targets")) return DropReason::NoTargets;
    if (is("target")) return DropReason::Target;
    if (is("quads")) return DropReason::Quads;
    if (is("pipeline:no-program")) return DropReason::NoProgram;
    if (is("pipeline:")) return DropReason::PipelineFailed;
    if (is("stream-full")) return DropReason::StreamFull;
    if (is("buffer-or-image-descriptors")) return DropReason::BufferOrImageDescriptors;
    if (is("depth-sample-dump")) return DropReason::DepthSampleDump;
    if (is("floor-d32-mirror")) return DropReason::FloorD32Mirror;
    if (is("feedback-snapshot")) return DropReason::FeedbackSnapshot;
    if (is("image-view")) return DropReason::ImageView;
    if (is("vertex-buffer")) return DropReason::VertexBuffer;
    if (is("vertex-unmapped")) return DropReason::VertexUnmapped;
    if (is("index-buffer")) return DropReason::IndexBuffer;
    if (is("index-unmapped")) return DropReason::IndexUnmapped;
    if (is("push-descriptor-proc")) return DropReason::PushDescriptorProc;
    if (is("descriptor-pool")) return DropReason::DescriptorPool;
    return DropReason::Other;
}

// Host input sampled once per frame for the frames CSV (sticks -32767..32767).
struct CensusInput {
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint64_t buttons = 0;
};

class DrawCensus {
public:
    static constexpr size_t kFrameFlushLines = 16;
    using InputProvider = std::function<CensusInput()>;

    static constexpr size_t kFlushLines = 32;

    // path: draw CSV; counters go to <path>.counters.csv. A path that cannot be opened leaves the census disabled.
    explicit DrawCensus(const std::string& path) {
        draws_ = std::fopen(path.c_str(), "w");
        counters_ = std::fopen((path + ".counters.csv").c_str(), "w");
        if (draws_) {
            std::string header = "frame,prog,game_draws,exec_draws,dropped,exec_indexed,exec_instanced,"
                                 "exec_zero_count,exec_empty_vp_sc";
            for (const char* n : kDropReasonNames) header += std::string(",reason_") + n;
            header += "\n";
            std::fputs(header.c_str(), draws_);
        }
        if (counters_) std::fputs("frame,type,gpu_addr,value,mapped\n", counters_);
        frames_ = std::fopen((path + ".frames.csv").c_str(), "w");
        if (frames_)
            std::fputs("frame,compute_dispatches,compute_writeback_bytes,compute_programs,game_draws_total,"
                       "exec_draws_total,stick_lx,stick_ly,stick_rx,stick_ry,buttons\n", frames_);
    }
    void SetInputProvider(InputProvider p) { input_provider_ = std::move(p); }
    ~DrawCensus() {
        Flush();
        if (draws_) std::fclose(draws_);
        if (counters_) std::fclose(counters_);
        if (frames_) std::fclose(frames_);
    }
    DrawCensus(const DrawCensus&) = delete;
    DrawCensus& operator=(const DrawCensus&) = delete;
    bool Ok() const { return draws_ != nullptr; }

    // The game requested a draw (call at Draw entry). Becomes the "current" draw for Exec/Drop.
    void GameDraw(uint64_t frame, uint64_t prog) {
        Advance(frame);
        cur_ = &frame_rows_[prog];
        ++cur_->game;
    }
    // A compute dispatch of guest program `prog` (writeback bytes are added separately).
    void ComputeDispatch(uint64_t frame, uint64_t prog) {
        Advance(frame);
        ++compute_dispatches_;
        ++compute_progs_[prog];
    }
    void ComputeWriteback(uint64_t bytes) { compute_bytes_ += bytes; }
    void Exec(bool indexed, bool instanced, bool zero_count, bool empty_vp_sc) {
        if (!cur_) return;
        ++cur_->exec;
        cur_->indexed += indexed;
        cur_->instanced += instanced;
        cur_->zero_count += zero_count;
        cur_->empty_vp_sc += empty_vp_sc;
    }
    void Drop(std::string_view why) {
        if (!cur_) return;
        ++cur_->dropped;
        ++cur_->reasons[static_cast<size_t>(ClassifyDropReason(why))];
    }
    void Counter(uint64_t frame, uint64_t type, uint64_t gpu_addr, uint64_t value, bool mapped) {
        Advance(frame);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%llu,%llu,%llx,%llx,%d\n", static_cast<unsigned long long>(frame),
                      static_cast<unsigned long long>(type), static_cast<unsigned long long>(gpu_addr),
                      static_cast<unsigned long long>(value), mapped ? 1 : 0);
        counter_lines_.emplace_back(buf);
        if (counter_lines_.size() >= kFlushLines) FlushCounters();
    }
    // Finishes the open frame (rows become CSV lines) and writes everything pending.
    void Flush() {
        FinishFrame();
        FlushDraws();
        FlushCounters();
        FlushFrames();
    }
    // One summary line each time a frame multiple of 600 starts; empty otherwise. Consumed by the call.
    std::string TakeSummary() { return std::exchange(summary_, std::string{}); }

private:
    struct Row {
        uint64_t game = 0, exec = 0, dropped = 0, indexed = 0, instanced = 0, zero_count = 0, empty_vp_sc = 0;
        std::array<uint64_t, static_cast<size_t>(DropReason::Count)> reasons{};
    };

    void Advance(uint64_t frame) {
        if (!have_frame_) {
            have_frame_ = true;
            frame_ = frame;
            SampleInput();
        } else if (frame != frame_) {
            FinishFrame();
            frame_ = frame;
            SampleInput();
            if (frame % 600 == 0) MakeSummary(frame);
        }
    }
    void SampleInput() {
        frame_open_ = true; input_ = input_provider_ ? input_provider_() : CensusInput{}; }
    void FinishFrame() {
        cur_ = nullptr;
        if (!frame_open_) return;
        frame_open_ = false;
        uint64_t frame_game = 0, frame_exec = 0;
        for (const auto& kv : frame_rows_) {
            frame_game += kv.second.game;
            frame_exec += kv.second.exec;
        }
        if (frames_) {
            std::string progs;
            for (const auto& kv : compute_progs_) {
                char h[24];
                std::snprintf(h, sizeof h, "%016llx", static_cast<unsigned long long>(kv.first));
                if (!progs.empty()) progs += ';';
                progs += h;
            }
            char buf[256];
            std::snprintf(buf, sizeof buf, "%llu,%llu,%llu,", static_cast<unsigned long long>(frame_),
                          static_cast<unsigned long long>(compute_dispatches_),
                          static_cast<unsigned long long>(compute_bytes_));
            std::string line = buf + progs;
            std::snprintf(buf, sizeof buf, ",%llu,%llu,%d,%d,%d,%d,%llx\n", static_cast<unsigned long long>(frame_game),
                          static_cast<unsigned long long>(frame_exec), input_.lx, input_.ly, input_.rx, input_.ry,
                          static_cast<unsigned long long>(input_.buttons));
            frame_lines_.push_back(line + buf);
            if (frame_lines_.size() >= kFrameFlushLines) FlushFrames();
        }
        compute_dispatches_ = 0;
        compute_bytes_ = 0;
        compute_progs_.clear();
        for (const auto& [prog, r] : frame_rows_) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "%llu,%016llx,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
                          static_cast<unsigned long long>(frame_), static_cast<unsigned long long>(prog),
                          static_cast<unsigned long long>(r.game), static_cast<unsigned long long>(r.exec),
                          static_cast<unsigned long long>(r.dropped), static_cast<unsigned long long>(r.indexed),
                          static_cast<unsigned long long>(r.instanced), static_cast<unsigned long long>(r.zero_count),
                          static_cast<unsigned long long>(r.empty_vp_sc));
            std::string line = buf;
            for (uint64_t n : r.reasons) line += "," + std::to_string(n);
            line += "\n";
            draw_lines_.push_back(std::move(line));
            total_game_ += r.game;
            total_exec_ += r.exec;
            total_dropped_ += r.dropped;
            for (size_t i = 0; i < r.reasons.size(); ++i) total_reasons_[i] += r.reasons[i];
        }
        frame_rows_.clear();
        if (draw_lines_.size() >= kFlushLines) FlushDraws();
    }
    void MakeSummary(uint64_t frame) {
        std::string s = "census: q=" + std::to_string(frame) + " game=" + std::to_string(total_game_) +
                        " exec=" + std::to_string(total_exec_) + " dropped=" + std::to_string(total_dropped_);
        for (size_t i = 0; i < total_reasons_.size(); ++i) {
            if (total_reasons_[i]) s += std::string(" ") + kDropReasonNames[i] + "=" + std::to_string(total_reasons_[i]);
        }
        summary_ = std::move(s);
    }
    void FlushDraws() {
        if (!draws_) return;
        for (const auto& l : draw_lines_) std::fputs(l.c_str(), draws_);
        draw_lines_.clear();
        std::fflush(draws_);
    }
    void FlushCounters() {
        if (!counters_) return;
        for (const auto& l : counter_lines_) std::fputs(l.c_str(), counters_);
        counter_lines_.clear();
        std::fflush(counters_);
    }

    void FlushFrames() {
        if (!frames_) return;
        for (const auto& l : frame_lines_) std::fputs(l.c_str(), frames_);
        frame_lines_.clear();
        std::fflush(frames_);
    }

    bool frame_open_ = false;
    FILE* frames_ = nullptr;
    InputProvider input_provider_;
    CensusInput input_;
    uint64_t compute_dispatches_ = 0, compute_bytes_ = 0;
    std::map<uint64_t, uint64_t> compute_progs_;
    std::vector<std::string> frame_lines_;
    FILE* draws_ = nullptr;
    FILE* counters_ = nullptr;
    bool have_frame_ = false;
    uint64_t frame_ = 0;
    std::map<uint64_t, Row> frame_rows_;
    Row* cur_ = nullptr;
    std::vector<std::string> draw_lines_, counter_lines_;
    uint64_t total_game_ = 0, total_exec_ = 0, total_dropped_ = 0;
    std::array<uint64_t, static_cast<size_t>(DropReason::Count)> total_reasons_{};
    std::string summary_;
};

} // namespace nxrt::render
