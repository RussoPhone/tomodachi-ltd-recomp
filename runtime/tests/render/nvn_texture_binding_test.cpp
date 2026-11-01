// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "memory.h"
#include "nvn/nvn_layer.h"
#include "nvn/recorder.h"
#include "nvn/texture_copy.h"
#include "replay/capturing_renderer.h"
#include "render/rendered_srgb_content.h"

// Replace only the NVN interception transport; observers and guest memory are production code.
namespace {
struct Observers { nxrt::nvn::Observer entry, returned; };
std::map<std::string, Observers> observers;
int failures = 0;
void Check(bool condition, const char* label) {
    if (!condition) { std::printf("FAIL: %s\n", label); ++failures; }
}
bool Invoke(const char* name, std::initializer_list<u64> args, u64 result = 0) {
    auto it = observers.find(name);
    if (it == observers.end()) return false;
    nxrt::nvn::Call call{};
    size_t i = 0;
    for (u64 arg : args) call.x[i++] = arg;
    if (it->second.entry) it->second.entry(call);
    call.ret = result;
    if (it->second.returned) it->second.returned(call);
    return true;
}
}
namespace nxrt::nvn {
u64 DriverModuleBase() { return 0; }
void Observe(const std::string& name, Observer entry, Observer returned) {
    observers[name] = {std::move(entry), std::move(returned)};
}

namespace {
using TextureTable = nxrt::nvn::TextureBindings;
class BindingRenderer final : public nxrt::nvn::Renderer {
public:
    TextureTable table{};
    std::vector<TextureTable> draws;
    nxrt::nvn::CommandList last_submission;
    std::shared_ptr<nxrt::nvn::Model> last_model;
    std::vector<std::shared_ptr<nxrt::nvn::Model>> submitted_models;
    void ExecuteSubmission(const nxrt::nvn::SubmissionSnapshot& snapshot) override {
        last_model = snapshot.execution_model;
        submitted_models.push_back(last_model);
        Execute(snapshot.list);
    }
    void BlockNextExecution() {
        std::lock_guard guard(sync_mutex);
        block_next = true;
        entered = false;
        released = false;
    }
    void WaitUntilExecutionBlocked() {
        std::unique_lock guard(sync_mutex);
        sync_cv.wait(guard, [&] { return entered; });
    }
    void ReleaseExecution() {
        std::lock_guard guard(sync_mutex);
        released = true;
        sync_cv.notify_all();
    }
    void Execute(const nxrt::nvn::CommandList& list) override {
        {
            std::unique_lock guard(sync_mutex);
            last_submission = list;
            if (block_next) {
                block_next = false;
                entered = true;
                sync_cv.notify_all();
                sync_cv.wait(guard, [&] { return released; });
            }
        }
        for (const auto& command : list.commands) {
            if (command.op == nxrt::nvn::Op::DrawElements) {
                draws.push_back(table);
            } else if (command.op == nxrt::nvn::Op::CallCommands) {
                for (size_t i = 0; i + 1 < command.words.size(); i += 2) {
                    const u64 handle = command.words[i] | (u64{command.words[i + 1]} << 32);
                    Execute(*nxrt::nvn::GetModel().finished.at(handle));
                }
            } else {
                nxrt::nvn::ApplyTextureBinding(command, table, [](u64 address) -> std::optional<u64> {
                    if (!nxrt::g_memory.IsMapped(address, sizeof(u64))) return std::nullopt;
                    return nxrt::g_memory.Read<u64>(address);
                });
            }
        }
    }
    void Present(u64, u64) override {}
private:
    std::mutex sync_mutex;
    std::condition_variable sync_cv;
    bool block_next = false;
    bool entered = false;
    bool released = false;
};

class SubmissionProbeRenderer final : public nxrt::nvn::Renderer {
public:
    const std::uint32_t* first_words = nullptr;
    void Execute(const nxrt::nvn::CommandList& list) override {
        if (!list.commands.empty() && !list.commands.front().words.empty())
            first_words = list.commands.front().words.data();
    }
    void Present(u64, u64) override {}
};

class MappedRangeRenderer final : public nxrt::nvn::Renderer {
public:
    std::array<u64, 3> invalidated{};
    void Execute(const nxrt::nvn::CommandList&) override {}
    void Present(u64, u64) override {}
    bool InvalidateMappedRange(u64 pool, u64 offset, u64 size) override {
        invalidated = {pool, offset, size};
        return true;
    }
};

void TestCaptureForwardsSubmissionMetadata() {
    BindingRenderer backend;
    nxrt::replay::CaptureOptions options;
    options.first_frame = 100;
    options.frame_count = 1;
    nxrt::replay::CapturingRenderer wrapper(backend, options);
    nxrt::nvn::SubmissionSnapshot submission;
    submission.execution_model = std::make_shared<nxrt::nvn::Model>();
    wrapper.ExecuteSubmission(submission);
    Check(backend.last_model == submission.execution_model,
          "capture wrapper forwards the exact submitted metadata to its backend");
}

void TestActiveCaptureUsesSubmissionMetadata() {
    const auto parent = std::filesystem::temp_directory_path() /
        ("nxrt-submit-metadata-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(parent);
    nxrt::replay::CaptureOptions options;
    options.first_frame = 0;
    options.frame_count = 1;
    options.directory = parent / "capture";
    options.executable = parent / "runtime.bin";
    { std::ofstream file(options.executable); file << "test-runtime"; }
    nxrt::nvn::SubmissionSnapshot submission;
    submission.execution_model = std::make_shared<nxrt::nvn::Model>();
    submission.execution_model->textures[0xabc].serial = 1;
    GetModel().textures[0xabc].serial = 2;
    BindingRenderer backend;
    {
        nxrt::replay::CapturingRenderer wrapper(backend, options);
        wrapper.ExecuteSubmission(submission);
    }
    std::ifstream events(options.directory / "events.bin", std::ios::binary);
    const auto version = nxrt::replay::ReadEventsHeader(events);
    const auto state = nxrt::replay::ReadEvent(events, version);
    std::istringstream encoded(state.model_snapshot, std::ios::binary);
    const auto decoded = nxrt::replay::ReadModel(encoded);
    Check(decoded->textures.at(0xabc).serial == 1 && backend.last_model == submission.execution_model,
          "active capture serializes the submitted version and forwards the same metadata");
    GetModel().textures.erase(0xabc);
    std::filesystem::remove_all(parent);
}

void TestExecuteSubmissionDoesNotCopyCommandWords() {
    using namespace nxrt::nvn;
    SubmissionSnapshot snapshot;
    Command command{};
    command.op = Op::DrawElements;
    command.words = {0x12345678, 0xabcdef01};
    snapshot.list.commands.push_back(std::move(command));
    snapshot.list.origins.push_back({0x200010, 1});
    const std::uint32_t* const snapshot_words = snapshot.list.commands.front().words.data();

    SubmissionProbeRenderer renderer;
    renderer.ExecuteSubmission(snapshot);
    Check(renderer.first_words == snapshot_words,
          "default ExecuteSubmission must pass owned command storage through without a second deep copy");
}

void Submit(u64 list) {
    nxrt::g_memory.Write<u64>(0x100080, list);
    Invoke("nvnQueueSubmitCommands", {0x1234, 1, 0x100080});
}
}
void SetCommandBufferHooks(std::function<void(u64, const std::string&)>, std::function<void(u64)>) {}
float Call::F(int i) const { float value; const u32 bits = static_cast<u32>(v[i]); std::memcpy(&value, &bits, 4); return value; }
}

int main() {
    nxrt::nvn::TestCaptureForwardsSubmissionMetadata();
    nxrt::nvn::TestActiveCaptureUsesSubmissionMetadata();
    using namespace nxrt;
    using namespace nxrt::nvn;
    TestExecuteSubmissionDoesNotCopyCommandWords();
    if (!g_memory.Init() || !g_memory.Map(0x100000, 0x1000, MemState::Normal, PermRW)) return 2;
    InstallRecorder();
    {
        MappedRangeRenderer renderer;
        SetRenderer(&renderer);
        Check(Invoke("nvnMemoryPoolInvalidateMappedRange", {0x123400, 0x1f80, 0x1f80}),
              "mapped-memory invalidation is observed");
        Check(renderer.invalidated == std::array<u64, 3>{0x123400, 0x1f80, 0x1f80},
              "CPU readback reaches the renderer with its exact pool and byte range");
        SetRenderer(nullptr);
    }
    g_memory.Write<u64>(0x100000, 0x11be01053ULL);
    g_memory.Write<u64>(0x100008, 0x11c80749dULL);
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Check(Invoke("nvnCommandBufferBindTextures", {0x100100, 1, 2, 2, 0x100000}),
          "immediate bulk textures are recorded");
    Check(Invoke("nvnCommandBufferBindTexturesDeferred", {0x100100, 1, 6, 2, 0x100000}),
          "deferred bulk textures are recorded");
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200000);
    const auto& list = *GetModel().finished.at(0x200000);
    Check(list.commands.size() == 3, "two immediate slots and one deferred range preserve order");
    if (list.commands.size() == 3) {
        Check(list.commands[0].op == Op::BindTexture && list.commands[0].a[0] == 1 &&
                  list.commands[0].a[1] == 2 && list.commands[0].a[2] == 0x11be01053ULL &&
                  list.commands[1].a[1] == 3 && list.commands[1].a[2] == 0x11c80749dULL,
              "immediate range snapshots complete handles at stage one slots two and three");
        Check(list.commands[2].a[0] == 1 && list.commands[2].a[1] == 6 &&
                  list.commands[2].a[2] == 2 && list.commands[2].a[3] == 0x100000,
              "deferred range retains stage first count and guest CPU pointer");
    }
    // A recording-time snapshot of deferred handles is a bug: each submission must consume the current array.
    BindingRenderer renderer;
    for (auto& stage : renderer.table) stage.fill(0xdeadbeef);
    SetRenderer(&renderer);
    g_memory.Write<u64>(0x100000, 0x110206fbeULL);
    g_memory.Write<u64>(0x100008, 0x110206ac3ULL);
    Submit(0x200000);
    Check(renderer.table[1][2] == 0x11be01053ULL && renderer.table[1][3] == 0x11c80749dULL,
          "immediate handles stay snapshotted after guest array mutation");
    Check(renderer.table[1][6] == 0x110206fbeULL && renderer.table[1][7] == 0x110206ac3ULL,
          "deferred Execute consumes the mutated guest array");
    Check(renderer.table[0][6] == 0xdeadbeef && renderer.table[1][5] == 0xdeadbeef &&
              renderer.table[1][8] == 0xdeadbeef,
          "partial range preserves other stages and adjacent slots");
    g_memory.Write<u64>(0x100000, 0x120106cd7ULL);
    g_memory.Write<u64>(0x100008, 0x11cb074ceULL);
    Submit(0x200000);
    Check(renderer.table[1][6] == 0x120106cd7ULL && renderer.table[1][7] == 0x11cb074ceULL,
          "replaying a finished list rereads deferred handles");

    // Real recorded draw commands observe binds in order, including a nested finished list.
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Invoke("nvnCommandBufferBindTexturesDeferred", {0x100100, 1, 6, 2, 0x100000});
    Invoke("nvnCommandBufferDrawElements", {0x100100, 4, 1, 3, 0});
    Invoke("nvnCommandBufferBindTexture", {0x100100, 1, 7, 0x11ba06c4dULL});
    Invoke("nvnCommandBufferDrawElements", {0x100100, 4, 1, 3, 0});
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200008);
    g_memory.Write<u64>(0x100088, 0x200008);
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Invoke("nvnCommandBufferCallCommands", {0x100100, 1, 0x100088});
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200010);
    Submit(0x200010);
    Check(renderer.draws.size() == 2 && renderer.draws[0][1][7] == 0x11cb074ceULL &&
              renderer.draws[1][1][7] == 0x11ba06c4dULL,
          "nested list draws see deferred bind then ordered single-slot override");

    // The submit boundary owns a snapshot: replacing a child list and mutating its deferred array after
    // Execute begins must affect the next submit, not the command stream already handed to the renderer.
    renderer.draws.clear();
    for (auto& stage : renderer.table) stage.fill(0xdeadbeef);
    renderer.BlockNextExecution();
    std::thread submitter([&] { Submit(0x200010); });
    renderer.WaitUntilExecutionBlocked();
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Invoke("nvnCommandBufferBindTexturesDeferred", {0x100100, 1, 6, 2, 0x100000});
    Invoke("nvnCommandBufferDrawElements", {0x100100, 4, 1, 3, 0});
    Invoke("nvnCommandBufferBindTexture", {0x100100, 1, 7, 0x11ba06c4dULL});
    Invoke("nvnCommandBufferDrawElements", {0x100100, 4, 1, 3, 0});
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200008);
    g_memory.Write<u64>(0x100000, 0x130106cd7ULL);
    g_memory.Write<u64>(0x100008, 0x13cb074ceULL);
    renderer.ReleaseExecution();
    submitter.join();
    Check(renderer.draws.size() == 2 && renderer.draws[0][1][6] == 0x120106cd7ULL &&
              renderer.draws[0][1][7] == 0x11cb074ceULL && renderer.draws[1][1][7] == 0x11ba06c4dULL,
          "in-flight submission keeps its original child list and deferred handles");
    bool saw_flat_snapshot = true;
    bool saw_submit_time_handles = false;
    for (const auto& command : renderer.last_submission.commands) {
        if (command.op == Op::CallCommands) saw_flat_snapshot = false;
        if (command.op == Op::BindTexturesDeferred && command.words.size() == 4 &&
            command.words[0] == static_cast<u32>(0x120106cd7ULL) &&
            command.words[1] == static_cast<u32>(0x120106cd7ULL >> 32) &&
            command.words[2] == static_cast<u32>(0x11cb074ceULL) &&
            command.words[3] == static_cast<u32>(0x11cb074ceULL >> 32)) {
            saw_submit_time_handles = true;
        }
    }
    Check(saw_flat_snapshot && saw_submit_time_handles,
          "renderer receives a flattened submission with deferred handles materialized by value");
    Submit(0x200010);
    Check(renderer.draws.size() == 4 && renderer.draws[2][1][6] == 0x130106cd7ULL &&
              renderer.draws[3][1][7] == 0x11ba06c4dULL,
          "the next submission observes the replacement child and updated deferred array");

#if defined(_WIN32)
    auto set_snapshot_mode = [](const char* value) { _putenv_s("NXRT_EXECUTION_MODEL_SNAPSHOT", value); };
#else
    auto set_snapshot_mode = [](const char* value) {
        if (*value) setenv("NXRT_EXECUTION_MODEL_SNAPSHOT", value, 1);
        else unsetenv("NXRT_EXECUTION_MODEL_SNAPSHOT");
    };
#endif
    Check(!renderer.last_model, "default submissions do not capture model metadata");
    set_snapshot_mode("0");
    Submit(0x200010);
    Check(!renderer.last_model, "explicit zero keeps model capture disabled");
    auto& model = GetModel();
    model.programs[0x123].stages.push_back({0xabc, 0xdef});
    set_snapshot_mode("1");
    Submit(0x200010);
    Check(renderer.last_model && renderer.last_model->programs.at(0x123).stages[0].code == 0xabc,
          "enabled submit hands the renderer an owned model snapshot");
    model.programs[0x123].stages[0].code = 0x987;
    Check(renderer.last_model && renderer.last_model->programs.at(0x123).stages[0].code == 0xabc,
          "replacing live shader metadata leaves the submitted model intact");
    Submit(0x200010);
    Check(renderer.last_model && renderer.last_model->programs.at(0x123).stages[0].code == 0x987,
          "next submit captures the current shader version");
    g_memory.Write<u64>(0x100088, 0x200010);
    Invoke("nvnQueueSubmitCommands", {0x1234, 2, 0x100080});
    const auto captured = renderer.submitted_models.size();
    Check(captured >= 2 && renderer.submitted_models[captured - 1] &&
          renderer.submitted_models[captured - 1] == renderer.submitted_models[captured - 2],
          "all lists in one QueueSubmit share one metadata capture");
    set_snapshot_mode("");
    const TextureTable before = renderer.table;
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Invoke("nvnCommandBufferBindTexturesDeferred", {0x100100, 1, 31, 2, 0x100000});
    Invoke("nvnCommandBufferBindTexturesDeferred", {0x100100, 6, 0, 1, 0x100000});
    Invoke("nvnCommandBufferBindTexturesDeferred", {0x100100, 1, 0, 0, 0});
    Invoke("nvnCommandBufferBindTexturesDeferred", {0x100100, 1, 6, 2, 0x100ff8});
    Invoke("nvnCommandBufferBindTextures", {0x100100, 1, 31, 2, 0x100000});
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200018);
    Submit(0x200018);
    Check(renderer.table == before, "invalid ranges and unmapped deferred tail never partially replace bindings");
    SetRenderer(nullptr);
    // SetViewports is the floating-point array API used by the Mii compositor.
    // It must snapshot viewport zero, preserve fractions, and not treat the pointer as x/y/width/height.
    const std::array<float, 4> viewport{-0.25f, 1.5f, 384.0f, 384.0f};
    g_memory.WriteBlock(0x100200, viewport.data(), sizeof(viewport));
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Check(Invoke("nvnCommandBufferSetViewports", {0x100100, 0, 1, 0x100200}),
          "floating viewport array API has an observer");
    Invoke("nvnCommandBufferSetViewport", {0x100100, 0, 0, 1920, 1080});
    Invoke("nvnCommandBufferSetViewports", {0x100100, 1, 1, 0x100200});
    Invoke("nvnCommandBufferSetViewports", {0x100100, 0, 0, 0x100200});
    Invoke("nvnCommandBufferSetViewports", {0x100100, 0, 1, 0x100ff8});
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200020);
    g_memory.Write<float>(0x100200, 42.0f);
    const auto& viewports = GetModel().finished.at(0x200020)->commands;
    Check(viewports.size() == 2, "only a valid range containing viewport zero is recorded");
    if (viewports.size() == 2) {
        Check(viewports[0].op == Op::SetViewport && viewports[0].a[4] == 1 && viewports[0].f == viewport,
              "floating viewport is snapshotted exactly, including negative fractions");
        Check(viewports[1].op == Op::SetViewport && viewports[1].a[4] == 0 &&
                  viewports[1].a[2] == 1920 && viewports[1].a[3] == 1080,
              "legacy integer viewport remains ordered after the floating range");
    }
    // SetScissors (int rect array) and ClearTexture: both were dropped by the recorder, so face targets kept a stale
    // scissor and stale texels. Both must be observed, snapshot by value, and ordered with the other commands.
    const std::array<std::int32_t, 4> scissor{0, 0, 384, 384};
    g_memory.WriteBlock(0x100400, scissor.data(), sizeof(scissor));
    const std::array<std::uint32_t, 6> region{0, 0, 0, 384, 384, 1};
    g_memory.WriteBlock(0x100440, region.data(), sizeof(region));
    const std::array<float, 4> clear_color{0.0f, 0.25f, 0.5f, 1.0f};
    g_memory.WriteBlock(0x100480, clear_color.data(), sizeof(clear_color));
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Check(Invoke("nvnCommandBufferSetScissors", {0x100100, 0, 1, 0x100400}), "scissor array API has an observer");
    Invoke("nvnCommandBufferSetScissors", {0x100100, 1, 1, 0x100400});
    Invoke("nvnCommandBufferSetScissors", {0x100100, 0, 0, 0x100400});
    Invoke("nvnCommandBufferSetScissors", {0x100100, 0, 1, 0x100ff8});
    Check(Invoke("nvnCommandBufferClearTexture", {0x100100, 0x300000, 0, 0x100440, 0x100480, 0xf}),
          "ClearTexture has an observer");
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200030);
    g_memory.Write<std::uint32_t>(0x100400, 7);
    g_memory.Write<float>(0x100480, 42.0f);
    const auto& cmds = GetModel().finished.at(0x200030)->commands;
    Check(cmds.size() == 2, "only valid scissor range zero and the clear are recorded");
    if (cmds.size() == 2) {
        Check(cmds[0].op == Op::SetScissor && cmds[0].a[0] == 0 && cmds[0].a[1] == 0 && cmds[0].a[2] == 384 &&
                  cmds[0].a[3] == 384,
              "scissor rect is snapshotted by value");
        Check(cmds[1].op == Op::ClearTexture && cmds[1].a[0] == 0x300000 && cmds[1].a[1] == 0xf && cmds[1].a[2] == 1 &&
                  cmds[1].f == clear_color && cmds[1].words.size() == 8 && cmds[1].words[3] == 384 &&
                  cmds[1].words[4] == 384,
              "ClearTexture records texture, mask, color and region by value");
    }
    // The facepaint canvas is copied to a CPU-visible GPU buffer on confirmation.
    // Its stack-allocated source region must survive recording and later mutation.
    g_memory.WriteBlock(0x100440, region.data(), sizeof(region));
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Invoke("nvnCommandBufferSetScissor", {0x100100, 0, 0, 384, 384});
    Check(Invoke("nvnCommandBufferCopyTextureToBuffer", {0x100100, 0x300000, 0, 0x100440, 0x7000, 0}),
          "texture-to-buffer copy has an observer");
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200031);
    g_memory.Write<u32>(0x100440 + 12, 7);
    const auto& readback = GetModel().finished.at(0x200031)->commands;
    Check(readback.size() == 2, "texture-to-buffer copy stays ordered after earlier rendering state");
    if (readback.size() == 2) {
        Check(readback[1].a[0] == 0x300000 && readback[1].a[1] == 0x7000 &&
                  readback[1].words.size() == 8 && readback[1].words[3] == 384 && readback[1].words[4] == 384,
              "texture-to-buffer preserves destination GPU address and snapshots region/view");
    }
    // Array bind calls (nvnCommandBufferBindStorageBuffers / BindUniformBuffers): ranges are {address, size} pairs,
    // snapshotted by value, one command per range with the stage and the absolute slot.
    const std::array<std::uint64_t, 4> ranges{0x7000, 0x40, 0x9000, 0x80};
    g_memory.WriteBlock(0x100500, ranges.data(), sizeof(ranges));
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Check(Invoke("nvnCommandBufferBindStorageBuffers", {0x100100, 5, 1, 2, 0x100500}),
          "BindStorageBuffers has an observer");
    Check(Invoke("nvnCommandBufferBindUniformBuffers", {0x100100, 1, 3, 1, 0x100500}),
          "BindUniformBuffers has an observer");
    Invoke("nvnCommandBufferBindStorageBuffers", {0x100100, 5, 15, 2, 0x100500});  // 15 + 2 > 16: rejected
    Invoke("nvnCommandBufferBindStorageBuffers", {0x100100, 5, 0, 0, 0x100500});   // count 0: rejected
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200040);
    g_memory.Write<std::uint64_t>(0x100500, 0xdead);
    const auto& binds = GetModel().finished.at(0x200040)->commands;
    Check(binds.size() == 3, "two storage ranges and one uniform range are recorded; invalid ranges are dropped");
    if (binds.size() == 3) {
        Check(binds[0].op == Op::BindStorageBuffer && binds[0].a[0] == 5 && binds[0].a[1] == 1 &&
                  binds[0].a[2] == 0x7000 && binds[0].a[3] == 0x40,
              "first storage range keeps stage, slot, address and size by value");
        Check(binds[1].op == Op::BindStorageBuffer && binds[1].a[1] == 2 && binds[1].a[2] == 0x9000 &&
                  binds[1].a[3] == 0x80,
              "second storage range goes to the next slot");
        Check(binds[2].op == Op::BindUniformBuffer && binds[2].a[0] == 1 && binds[2].a[1] == 3 &&
                  binds[2].a[2] == 0x7000,
              "uniform range keeps stage and slot");
    }
    {
        Invoke("nvnTextureBuilderSetDefaults", {0x400000});
        Invoke("nvnTextureBuilderSetSize2D", {0x400000, 512, 512});
        Invoke("nvnTextureBuilderSetFormat", {0x400000, 66});
        g_memory.Write<u64>(0x100700, 0x123456789abcdef0);
        Check(Invoke("nvnTextureBuilderSetPackagedTextureLayout", {0x400000, 0x100700}),
              "packaged texture layout is observed by value");
        Check(Invoke("nvnTextureBuilderGetStorageSize", {0x400000}, 0x20000),
              "driver storage size is observed after the driver returns");
        Invoke("nvnTextureInitialize", {0x400100, 0x400000});
        Invoke("nvnTextureInitialize", {0x400200, 0x400000});
        Invoke("nvnCommandBufferBeginRecording", {0x100100});
        Invoke("nvnCommandBufferCopyTextureToTexture", {0x100100, 0x400100, 0, 0x100440, 0x400200, 0, 0x100440, 0});
        Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200032);
        const auto& recorded = GetModel().finished.at(0x200032)->commands;
        Check(recorded.size() == 1 && recorded[0].a[3] == 0x20000 && recorded[0].a[4] == 0x20000,
              "texture copy snapshots both driver sizes for later execution and replay");
        g_memory.Write<u64>(0x100700, 0);
        Check(recorded.size() == 1 && recorded[0].words.size() == 22 && recorded[0].words[16] == 1 &&
                  recorded[0].words[17] == 0x9abcdef0 && recorded[0].words[18] == 0x12345678 &&
                  recorded[0].words[19] == 1 && recorded[0].words[20] == 0x9abcdef0,
              "copy owns both packaged layouts after stack data changes");
        Model model;
        TextureDesc src{};
        src.width = src.height = 512; src.target = 1; src.format = 66; src.pool = 1; src.offset = 0x100;
        TextureDesc dst = src; dst.pool = 2; dst.offset = 0x200;
        model.textures[10].desc = src; model.textures[20].desc = dst;
        model.pools[1] = MemoryPool{0x100000, 0x20100};
        model.pools[2] = MemoryPool{0x200000, 0x20200};
        Command copy{Op::CopyTexture};
        copy.a = {10, 20, 0, 0x20000, 0x20000, 0};
        copy.words = {0,0,0,512,512,1, 0,0,0,512,512,1, 0,0,0,0, 0,0,0,0,0,0};
        const auto plan = PlanGuestTextureCopy(model, copy);
        Check(plan && plan->source == 0x100100 && plan->destination == 0x200200 && plan->size == 0x20000,
              "full CPU-backed texture copy transfers the driver-sized backing range");
        model.textures[10].desc.flags = 4;
        copy.a[3] = 0;
        Check(PlanGuestTextureCopy(model, copy).has_value(),
              "image access usage does not change storage layout; destination driver size bounds the copy");
        model.textures[10].desc.flags = 0;
        copy.a[3] = 0x20000;
        copy.words[16] = 1; copy.words[17] = 0x1234;
        Check(!PlanGuestTextureCopy(model, copy), "mixed packaged and default tiling is rejected");
        copy.words[19] = 1; copy.words[20] = 0x1235;
        Check(!PlanGuestTextureCopy(model, copy), "different packaged layouts cannot be copied as raw bytes");
        copy.words[20] = 0x1234;
        Check(PlanGuestTextureCopy(model, copy).has_value(), "equal packaged layout bytes preserve identical tiling");
        copy.words[16] = copy.words[19] = 0;
        copy.words[3] = 256;
        Check(!PlanGuestTextureCopy(model, copy), "partial compressed copy is not mistaken for a full backing copy");
        copy.words[3] = 512; model.pools[2].size--;
        Check(!PlanGuestTextureCopy(model, copy), "copy rejects a destination pool too short for the layout");
        model.pools[2].size++;
        model.textures[10].desc.target = model.textures[20].desc.target = 8;
        Check(!PlanGuestTextureCopy(model, copy), "copying one cubemap face cannot overwrite all six faces");
    }
    {
        nxrt::render::RenderedSrgbContent color;
        color.Draw(11, true, false);
        Check(color.Decode(true, false) && !color.Decode(false, false) && !color.Decode(true, true),
              "encoded palette content follows the TIC and legacy opt-out");
        color.Draw(11, false, true);
        Check(color.encoded, "same compositor can overlay through an inherited view");
        color.Draw(12, false, true, false);
        Check(color.encoded, "empty or RGB-masked draws preserve existing encoded pixels");
        auto copy = color;
        copy.Draw(12, false, true);
        Check(color.encoded && !copy.encoded, "unrelated successful writers invalidate encoded content");
        copy.CopyFrom(color, true);
        Check(copy.encoded && copy.writer == 11, "full copies propagate encoded palette provenance");
        copy.Draw(11, false, false);
        Check(!copy.encoded, "explicit contradictory view overrides same-writer retention");
        copy.CopyFrom(color, false);
        Check(!copy.encoded, "mixed partial copies cannot invent a uniform encoding convention");
        color.Clear();
        Check(!color.encoded, "clears invalidate palette provenance");
    }
    // A reusable UNORM view writes palette bytes into an sRGB facial texture.
    Invoke("nvnTextureViewSetDefaults", {0x660});
    Invoke("nvnTextureViewSetFormat", {0x660, 37});
    g_memory.Write<u64>(0x100900, 0x550);
    g_memory.Write<u64>(0x100908, 0x660);
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Invoke("nvnCommandBufferSetRenderTargets", {0x100100, 1, 0x100900, 0x100908, 0, 0});
    Invoke("nvnTextureViewSetFormat", {0x660, 56});
    Invoke("nvnCommandBufferSetRenderTargets", {0x100100, 1, 0x100900, 0x100908, 0, 0});
    Invoke("nvnTextureViewSetDefaults", {0x660});
    Invoke("nvnCommandBufferSetRenderTargets", {0x100100, 1, 0x100900, 0x100908, 0, 0});
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200050);
    const auto& targets = GetModel().finished.at(0x200050)->commands;
    Check(targets.size() == 3 && targets[0].words.size() == 7 && targets[0].words[6] == 37 &&
          targets[1].words.size() == 7 && targets[1].words[6] == 56 &&
          targets[2].words.size() == 7 && targets[2].words[6] == 0,
          "render target view formats survive reuse and reset to inherited on defaults");
    std::printf("%s (%d failures)\n", failures ? "FAILED" : "PASS", failures);
    return failures != 0;
}
