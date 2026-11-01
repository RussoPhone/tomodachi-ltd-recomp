// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <vector>

#include "log_macros.h"
#include "memory.h"
#include "nvn/recorder.h"
#include "nvn/nvn_layer.h"
#include "render/nvn_renderer.h"
#include "replay/capture_format.h"
#include "replay/replay_session.h"

namespace fs = std::filesystem;
using namespace nxrt;

namespace {

std::vector<replay::CaptureEvent> ReadEvents(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open events.bin");
    const u32 event_version = replay::ReadEventsHeader(input);
    std::vector<replay::CaptureEvent> events;
    while (input.peek() != std::char_traits<char>::eof()) {
        events.push_back(replay::ReadEvent(input, event_version));
    }
    if (!input.eof()) throw std::runtime_error("failed reading events.bin");
    if (events.empty()) throw std::runtime_error("capture contains no events");
    return events;
}

void MapPageAddresses(const replay::CapturePageReader& pages) {
    std::vector<u64> addresses;
    addresses.reserve(pages.references().size());
    for (const auto& ref : pages.references()) {
        if (ref.va >= AddressSpaceLayout::kEnd ||
            ref.va > std::numeric_limits<u64>::max() - replay::kCapturePageSize)
            throw std::runtime_error("capture page address is outside the guest address space");
        addresses.push_back(ref.va);
    }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    if (addresses.empty()) throw std::runtime_error("capture contains no mapped pages");

    std::size_t first = 0;
    while (first < addresses.size()) {
        const u64 begin = addresses[first];
        std::size_t end_index = first + 1;
        u64 end = begin + replay::kCapturePageSize;
        while (end_index < addresses.size() && addresses[end_index] == end) {
            end += replay::kCapturePageSize;
            ++end_index;
        }
        if (end > AddressSpaceLayout::kEnd ||
            !g_memory.Map(begin, end - begin, MemState::Normal, PermRW))
            throw std::runtime_error("could not map captured guest pages");
        first = end_index;
    }
    LOG_INFO("replay: mapped %zu guest pages", addresses.size());
}

void ApplyPages(const replay::CaptureEvent& event, replay::CapturePageReader& pages) {
    for (const auto& ref : event.pages) {
        if (!g_memory.IsMapped(ref.va, replay::kCapturePageSize))
            throw std::runtime_error("EXECUTE references an unmapped captured page");
        const auto bytes = pages.Read(ref.id);
        g_memory.WriteBlock(ref.va, bytes.data(), bytes.size());
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <capture directory>\n", argv[0]);
        return 2;
    }

    try {
        const fs::path directory = fs::absolute(argv[1]);
        if (!fs::is_directory(directory)) throw std::runtime_error("capture directory does not exist");
        const auto start = std::chrono::steady_clock::now();

        std::ifstream model_file(directory / "model.bin", std::ios::binary);
        if (!model_file) throw std::runtime_error("cannot open model.bin");
        replay::ReadModel(model_file, nvn::GetModel());
        if (!model_file.eof() && model_file.peek() != std::char_traits<char>::eof())
            throw std::runtime_error("model.bin has trailing data");

        replay::CapturePageReader pages(directory);
        const auto events = ReadEvents(directory / "events.bin");
        if (!g_memory.Init()) throw std::runtime_error("cannot initialize guest memory");
        MapPageAddresses(pages);

        auto renderer = render::CreateNvnRenderer(nullptr);
        if (!renderer) throw std::runtime_error("cannot initialize the offscreen Vulkan renderer");
        nvn::SetRenderer(renderer.get());

        const auto first_present = std::find_if(events.begin(), events.end(), [](const auto& event) {
            return event.type == replay::CaptureEvent::Type::Present;
        });
        if (first_present == events.end()) throw std::runtime_error("capture has no Present");
        replay::PrimeReplayFrameCounter(*renderer, first_present->frame);

        for (const auto& event : events) {
            if (event.type == replay::CaptureEvent::Type::Execute) {
                ApplyPages(event, pages);
                renderer->Execute(event.commands);
            } else if (event.type == replay::CaptureEvent::Type::State) {
                nvn::RestoreDriverModuleBaseForReplay(event.driver_base);
                std::istringstream state(event.model_snapshot, std::ios::binary);
                replay::ReadModel(state, nvn::GetModel());
            } else if (event.type == replay::CaptureEvent::Type::Present) {
                renderer->Present(event.texture, event.frame);
            } else {
                throw std::runtime_error("capture contains an unknown event type");
            }
        }
        nvn::SetRenderer(nullptr);
        renderer.reset();

        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        LOG_INFO("replay: %zu eventos em %.3f s", events.size(), elapsed);
        return 0;
    } catch (const std::exception& error) {
        nvn::SetRenderer(nullptr);
        LOG_ERROR("replay failed: %s", error.what());
        return 1;
    }
}
