#include "replay/capture_format.h"
#include "replay/capturing_renderer.h"
#include "replay/replay_session.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void TestCommandRoundTrip() {
    nxrt::nvn::Command input{};
    input.op = nxrt::nvn::Op::DrawElements;
    input.a = {1, 2, 3, 4, 5, 6};
    input.f = {0.25f, -1.5f, 2.0f, 99.0f};
    input.words = {0, 1, 0x12345678, 0xffffffff};
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    nxrt::replay::WriteCommand(stream, input);
    stream.seekg(0);
    const auto output = nxrt::replay::ReadCommand(stream);
    Check(output.op == input.op && output.a == input.a && output.f == input.f && output.words == input.words,
          "Command round trip differs");
    input.op = nxrt::nvn::Op::CopyTextureToBuffer;
    input.a = {0x203890be90, 0x695e80000, 0, 0, 0, 0};
    input.words = {0, 0, 0, 512, 512, 1, 0, 0};
    std::stringstream copy_stream(std::ios::in | std::ios::out | std::ios::binary);
    nxrt::replay::WriteCommand(copy_stream, input);
    copy_stream.seekg(0);
    const auto copy = nxrt::replay::ReadCommand(copy_stream);
    Check(copy.op == input.op && copy.a == input.a && copy.words == input.words,
          "Texture-to-buffer copy round trip differs");
}

void TestCommandListRoundTrip() {
    nxrt::nvn::CommandList input;
    nxrt::nvn::Command command{};
    command.op = nxrt::nvn::Op::SetViewport;
    command.a[0] = 0x88776655;
    input.commands.push_back(command);
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    nxrt::replay::WriteCommandList(stream, input);
    stream.seekg(0);
    const auto output = nxrt::replay::ReadCommandList(stream);
    Check(output.commands.size() == 1 && output.commands[0].op == command.op &&
              output.commands[0].a == command.a,
          "CommandList round trip differs");
}

void TestEventRoundTrip() {
    nxrt::replay::CaptureEvent input;
    input.type = nxrt::replay::CaptureEvent::Type::Execute;
    nxrt::nvn::Command command{};
    command.op = nxrt::nvn::Op::DrawArrays;
    command.a[2] = 42;
    input.commands.commands.push_back(command);
    input.commands.origins.push_back({0x123456789ULL, 2});
    input.pages = {{0x1000, 7}, {0x2000, 9}};
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    nxrt::replay::WriteEventsHeader(stream);
    nxrt::replay::WriteEvent(stream, input);
    stream.seekg(0);
    const auto version = nxrt::replay::ReadEventsHeader(stream);
    const auto output = nxrt::replay::ReadEvent(stream, version);
    Check(output.type == input.type && output.commands.commands.size() == 1 &&
              output.commands.commands[0].a[2] == 42 && output.commands.origins == input.commands.origins &&
              output.pages == input.pages,
          "EXECUTE event round trip differs");

    nxrt::replay::CaptureEvent present;
    present.type = nxrt::replay::CaptureEvent::Type::Present;
    present.texture = 0xabc;
    present.frame = 11000;
    std::stringstream present_stream(std::ios::in | std::ios::out | std::ios::binary);
    nxrt::replay::WriteEvent(present_stream, present);
    present_stream.seekg(0);
    const auto present_out = nxrt::replay::ReadEvent(present_stream);
    Check(present_out.type == present.type && present_out.texture == present.texture && present_out.frame == present.frame,
          "PRESENT event round trip differs");
}

void TestLegacyEventsHeaders() {
    for (const auto& [magic, expected] : {std::pair{"NXEVT001", 1u}, std::pair{"NXEVT002", 2u}}) {
        std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
        stream.write(magic, 8);
        stream.put(static_cast<char>(nxrt::replay::CaptureEvent::Type::Execute));
        nxrt::nvn::CommandList commands;
        nxrt::nvn::Command draw{};
        draw.op = nxrt::nvn::Op::DrawArrays;
        draw.a[2] = 17;
        commands.commands.push_back(draw);
        nxrt::replay::WriteCommandList(stream, commands);
        const auto write_u64 = [&](u64 value) {
            for (unsigned byte = 0; byte < 8; ++byte)
                stream.put(static_cast<char>((value >> (byte * 8)) & 0xff));
        };
        write_u64(1);  // page count
        write_u64(0x2000);
        write_u64(3);
        stream.put(static_cast<char>(nxrt::replay::CaptureEvent::Type::Present));
        write_u64(0xdeadbeef);
        write_u64(29);
        stream.seekg(0);
        const auto version = nxrt::replay::ReadEventsHeader(stream);
        const auto event = nxrt::replay::ReadEvent(stream, version);
        const auto present = nxrt::replay::ReadEvent(stream, version);
        Check(version == expected && event.type == nxrt::replay::CaptureEvent::Type::Execute &&
                  event.commands.commands.size() == 1 && event.commands.commands[0].a[2] == 17 &&
                  event.commands.origins.empty() && event.pages == std::vector<nxrt::replay::PageRef>{{0x2000, 3}},
              "legacy EXECUTE event did not decode with its header version");
        Check(present.type == nxrt::replay::CaptureEvent::Type::Present && present.texture == 0xdeadbeef &&
                  present.frame == 29,
              "legacy event reader lost alignment after EXECUTE");
    }
}

void TestMaterializedDeferredBindingHasNoGuestPointerDependency() {
    nxrt::nvn::Model model;
    nxrt::nvn::CommandList submitted;
    nxrt::nvn::Command deferred{};
    deferred.op = nxrt::nvn::Op::BindTexturesDeferred;
    deferred.a[0] = 1;
    deferred.a[1] = 3;
    deferred.a[2] = 1;
    deferred.a[3] = 0xdead0000;
    deferred.a[4] = 1;  // Submit-time handles are embedded in words; a[3] is stale diagnostic metadata.
    deferred.a[5] = 1;
    deferred.words = {0x89abcdef, 0x12345678};
    submitted.commands.push_back(deferred);
    const auto pages = nxrt::replay::ExecutionCpuReadPages(model, submitted);
    Check(pages.empty(), "materialized deferred binding retained a dependency on its stale guest pointer");
}

void TestModelRoundTrip() {
    nxrt::nvn::Model input;
    input.pool_generation = 17;
    input.pools.emplace(0x1000, nxrt::nvn::MemoryPool{0x2000, 0x3000, 4, 0x4000, 0x5000, 6, true});
    input.virtual_mappings.emplace(0x6000, nxrt::nvn::VirtualMapping{0x7000, 0x8000, 0x9000, 10, 0xa000, 11, 12});
    input.textures.emplace(0xb000, nxrt::nvn::Texture{{1, 2, 3, 4, 5, 6, 7, 8, {9, 10, 11, 12}, 13, 14,
                                                       0xc000, 15, 0xd000}, 16});
    input.programs.emplace(0xe000, nxrt::nvn::Program{{{0xf000, 0x11000}, {0x12000, 0x13000}}, 18});
    input.texture_pools[0x14000][19] = {0x15000, 0x16000};
    input.texture_pool_views[0x14000][19] = {20, 21, 22, 23, 37};
    input.windows[0x17000] = {{0x18000, 0x19000}, 1};
    auto nested = std::make_shared<nxrt::nvn::CommandList>();
    nxrt::nvn::Command nested_command{};
    nested_command.op = nxrt::nvn::Op::ClearColor;
    nested_command.words = {24, 25};
    nested->commands.push_back(nested_command);
    input.finished.emplace(0x1a000, nested);
    input.texture_pool_registration = {26, 27, 28};

    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    nxrt::replay::WriteModel(stream, input);
    stream.seekg(0);
    auto output = nxrt::replay::ReadModel(stream);
    Check(output->pool_generation == 17 && output->pools.size() == 1 && output->virtual_mappings.size() == 1,
          "Model pool fields round trip differs");
    Check(output->textures.size() == 1 && output->programs.size() == 1 && output->texture_pools.size() == 1 &&
              output->texture_pool_views.size() == 1,
          "Model texture/program fields round trip differs");
    Check(output->texture_pool_views.at(0x14000).at(19).format == 37,
          "Model preserves an explicit texture view format");
    Check(output->windows.size() == 1 && output->finished.size() == 1 && output->finished.at(0x1a000)->commands.size() == 1,
          "Model window/finished fields round trip differs");
    Check(output->texture_pool_registration.new_entries == 26 &&
              output->texture_pool_registration.changed_entries == 27 &&
              output->texture_pool_registration.unchanged_entries == 28,
          "Model registration stats round trip differs");
    Check(output->lock.try_lock(), "ReadModel must construct a fresh unlocked mutex");
    output->lock.unlock();
    // Old captures have four view words and inherit the texture format.
    std::string legacy = "NXMDL001";
    auto append = [&](u64 value, unsigned bytes = 8) {
        for (unsigned b = 0; b < bytes; ++b) legacy.push_back(static_cast<char>(value >> (8*b)));
    };
    append(17);
    for (unsigned i = 0; i < 5; ++i) append(0);
    append(1); append(0x14000); append(1); append(19, 4);
    for (u32 word : {20u, 21u, 22u, 23u}) append(word, 4);
    for (unsigned i = 0; i < 5; ++i) append(0);
    std::istringstream legacy_stream(legacy, std::ios::binary);
    auto old = nxrt::replay::ReadModel(legacy_stream);
    Check(old->texture_pool_views.at(0x14000).at(19).num_layers == 23 &&
          old->texture_pool_views.at(0x14000).at(19).format == 0,
          "Legacy model snapshots retain inherited view formats");
}

void TestPageStoreDeduplicates() {
    const auto dir = std::filesystem::temp_directory_path() / "nxrt-capture-format-test";
    std::filesystem::remove_all(dir);
    std::array<std::byte, nxrt::replay::kCapturePageSize> page{};
    page[0] = std::byte{0x5a};
    u64 first = 0;
    {
        nxrt::replay::PageStore store(dir);
        first = store.Put(0x1000, page);
        const auto duplicate = store.Put(0x9000, page);
        Check(first == duplicate, "identical pages must share an id");
        Check(store.Get(first) == std::vector<std::byte>(page.begin(), page.end()), "PageStore read differs");
        Check(store.unique_pages() == 1 && store.references().size() == 2, "page dedup/index count differs");
        store.Close();
    }
    Check(std::filesystem::file_size(dir / "pages.bin") == nxrt::replay::kCapturePageSize,
          "duplicate page was written more than once");
    Check(std::filesystem::file_size(dir / "pages.idx") > 0, "page index was not written");
    nxrt::replay::CapturePageReader reader(dir);
    Check(reader.references().size() == 2, "capture page reader lost VA references");
    const auto read_page = reader.Read(first);
    Check(std::equal(read_page.begin(), read_page.end(), page.begin(), page.end()),
          "capture page reader returned different page bytes");
    std::filesystem::remove_all(dir);
}

void TestReadModelIntoExistingModel() {
    nxrt::nvn::Model input;
    input.pool_generation = 31;
    input.pools.emplace(0x1000, nxrt::nvn::MemoryPool{0x2000, 0x3000, 4, 0x4000, 0x5000, 6, true});
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    nxrt::replay::WriteModel(stream, input);
    stream.seekg(0);
    nxrt::nvn::Model output;
    output.pools.emplace(0xdead, nxrt::nvn::MemoryPool{});
    nxrt::replay::ReadModel(stream, output);
    Check(output.pool_generation == 31 && output.pools.size() == 1 && output.pools.contains(0x1000) &&
              !output.pools.contains(0xdead),
          "ReadModel did not restore into the renderer's Model");
}

void TestMemoryPoolObjectPages() {
    const std::vector<std::uint64_t> handles{0x1fe8, 0x2ff0};
    const auto pages = nxrt::replay::MemoryPoolObjectPages(handles);
    Check(pages == std::vector<std::uint64_t>{0x2000, 0x3000},
          "memory pool object metadata pages were not aligned and deduplicated");
}

void TestCaptureWindowPreRoll() {
    nxrt::replay::CaptureOptions options;
    options.first_frame = 10998;
    options.frame_count = 3;
    Check(options.ShouldStartBeforePresent(10997), "capture must arm before the first target frame executes");
    Check(!options.ShouldStartBeforePresent(10998), "capture pre-roll must not trigger twice");
    Check(!options.IncludesPresent(10997) && options.IncludesPresent(10998) &&
              options.IncludesPresent(11000) && !options.IncludesPresent(11001),
          "capture must record exactly the requested Present frames");
    nxrt::replay::CaptureOptions from_zero;
    Check(from_zero.ShouldStartOnFirstExecute(), "frame-zero capture must arm before its first Execute");
}

void TestExecutionStateCapturesLateNestedLists() {
    nxrt::nvn::Model live;
    nxrt::nvn::Command call{};
    call.op = nxrt::nvn::Op::CallCommands;
    call.words = {0x2222, 0};
    nxrt::nvn::CommandList submitted;
    submitted.commands.push_back(call);
    auto late = std::make_shared<nxrt::nvn::CommandList>();
    nxrt::nvn::Command draw{};
    draw.op = nxrt::nvn::Op::DrawElements;
    draw.a[2] = 1350;
    late->commands.push_back(draw);
    live.finished.emplace(0x2222, late);
    live.finished.emplace(0xdead, std::make_shared<nxrt::nvn::CommandList>());
    live.textures.emplace(0x3333, nxrt::nvn::Texture{});
    nxrt::replay::CaptureEvent event;
    event.type = nxrt::replay::CaptureEvent::Type::State;
    event.driver_base = 0x7100000000;
    event.model_snapshot = nxrt::replay::SnapshotExecutionModel(live, submitted);
    late->commands.clear();
    live.textures.clear();
    std::stringstream wire(std::ios::in | std::ios::out | std::ios::binary);
    nxrt::replay::WriteEvent(wire, event);
    wire.seekg(0);
    const auto decoded = nxrt::replay::ReadEvent(wire);
    Check(decoded.driver_base == 0x7100000000, "execution snapshot lost the driver module base");
    std::istringstream model_stream(decoded.model_snapshot, std::ios::binary);
    auto restored = nxrt::replay::ReadModel(model_stream);
    Check(restored->textures.contains(0x3333) && restored->finished.size() == 1 &&
          restored->finished.at(0x2222)->commands.at(0).a[2] == 1350,
          "execution snapshot lost late resources or nested commands, or retained unrelated lists");
    submitted.commands[0].words = {0x9999, 0};
    bool rejected = false;
    try { nxrt::replay::SnapshotExecutionModel(live, submitted); }
    catch (const std::runtime_error&) { rejected = true; }
    Check(rejected, "capture must reject missing nested lists instead of silently skipping them");
}

void TestNestedDescriptorPoolsWithoutRegistration() {
    nxrt::nvn::Model model;
    auto nested = std::make_shared<nxrt::nvn::CommandList>();
    nxrt::nvn::Command set{};
    set.op = nxrt::nvn::Op::SetSamplerPool;
    set.a[0] = 0x1ff8;
    nested->commands.push_back(set);
    model.finished[0x2222] = nested;
    nxrt::nvn::Command call{};
    call.op = nxrt::nvn::Op::CallCommands;
    call.words = {0x2222, 0};
    nxrt::nvn::CommandList root;
    root.commands.push_back(call);
    const auto objects = nxrt::replay::SubmissionDescriptorPoolObjects(model, root);
    Check(objects == std::vector<u64>{0x1ff8},
          "nested pool headers must be discovered even without Model registration");
}

void TestCpuDependenciesOutsidePools() {
    nxrt::nvn::Model model;
    model.programs[0x1234].stages.push_back({0x8888, 0x10ff0});
    auto nested = std::make_shared<nxrt::nvn::CommandList>();
    nxrt::nvn::Command deferred{};
    deferred.op = nxrt::nvn::Op::BindTexturesDeferred;
    deferred.a = {1, 0, 2, 0x9ff8, 0, 0};
    nested->commands.push_back(deferred);
    model.finished[0x2222] = nested;
    nxrt::nvn::Command call{};
    call.op = nxrt::nvn::Op::CallCommands;
    call.words = {0x2222, 0};
    nxrt::nvn::CommandList root;
    root.commands.push_back(call);
    const auto pages = nxrt::replay::ExecutionCpuReadPages(model, root);
    Check(pages == std::vector<u64>{0x9000, 0xa000, 0x10000, 0x11000},
          "nested deferred bindings and shader controls outside pools must be captured");
}

void TestLargeStateEventRoundTrip() {
    nxrt::replay::CaptureEvent state;
    state.type = nxrt::replay::CaptureEvent::Type::State;
    state.model_snapshot.assign((1u << 24) + 1, 'x');
    std::stringstream stream(std::ios::in | std::ios::out | std::ios::binary);
    nxrt::replay::WriteEvent(stream, state);
    stream.seekg(0);
    const auto restored = nxrt::replay::ReadEvent(stream);
    Check(restored.model_snapshot == state.model_snapshot,
          "a successfully written large state must remain readable");
}

void TestDescriptorPoolObjectPages() {
    const std::vector<u64> objects{0x1ff8, 0x2000};
    const auto pages = nxrt::replay::DescriptorPoolObjectPages(objects);
    Check(pages == std::vector<u64>{0x1000, 0x2000},
          "descriptor pool object header must be captured even across page boundaries");
    const auto backing = nxrt::replay::MemoryPoolObjectPages(std::vector<u64>{0x3fd8});
    Check(backing == std::vector<u64>{0x3000, 0x4000},
          "memory pool CPU storage pointer at +0x30 must be captured");
}

void TestDriverTablePages() {
    const auto pages = nxrt::replay::DriverTablePages(0x1000000, 0xb69208, 0x915550, 0x3000000);
    Check(pages.size() == 26 && std::find(pages.begin(), pages.end(), 0x1b69000) != pages.end() &&
          std::find(pages.begin(), pages.end(), 0x1915000) != pages.end() &&
          std::find(pages.begin(), pages.end(), 0x3000000) != pages.end() &&
          std::find(pages.begin(), pages.end(), 0x3017000) != pages.end(),
          "driver format and SSBO tables must be included outside memory pools");
}

void TestReplayPrimesFrameCounter() {
    struct Counter final : nxrt::nvn::Renderer {
        u64 count = 0, last = 0;
        void Execute(const nxrt::nvn::CommandList&) override {}
        void Present(nxrt::nvn::GuestAddr texture, u64 frame) override {
            Check(texture == 0 && frame == count + 1, "priming must not render a guest texture");
            ++count; last = frame;
        }
    } renderer;
    nxrt::replay::PrimeReplayFrameCounter(renderer, 10998);
    Check(renderer.count == 10997 && renderer.last == 10997,
          "first replay draw must retain its original frame number");
}

void TestManifestHash() {
    const auto dir = std::filesystem::temp_directory_path() / "nxrt-capture-manifest-test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto fixture = dir / "fixture.bin";
    { std::ofstream out(fixture, std::ios::binary); out << "abc"; }
    Check(nxrt::replay::Sha256File(fixture) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 fixture mismatch");
    nxrt::replay::CaptureManifest manifest{1, 10998, 3, "2026-10-17T12:00:00Z", "abcd"};
    nxrt::replay::WriteManifest(dir / "manifest.json", manifest);
    std::ifstream in(dir / "manifest.json");
    const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Check(json.find("\"first\": 10998") != std::string::npos && json.find("\"count\": 3") != std::string::npos &&
              json.find(manifest.date) != std::string::npos && json.find(manifest.executable_sha256) != std::string::npos,
          "manifest fields missing");
    std::filesystem::remove_all(dir);
}
} // namespace

int main() {
    try {
        TestCommandRoundTrip();
        TestCommandListRoundTrip();
        TestEventRoundTrip();
        TestLegacyEventsHeaders();
        TestMaterializedDeferredBindingHasNoGuestPointerDependency();
        TestModelRoundTrip();
        TestPageStoreDeduplicates();
        TestReadModelIntoExistingModel();
        TestMemoryPoolObjectPages();
        TestCaptureWindowPreRoll();
        TestExecutionStateCapturesLateNestedLists();
        TestNestedDescriptorPoolsWithoutRegistration();
        TestCpuDependenciesOutsidePools();
        TestLargeStateEventRoundTrip();
        TestDescriptorPoolObjectPages();
        TestDriverTablePages();
        TestReplayPrimesFrameCounter();
        TestManifestHash();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
