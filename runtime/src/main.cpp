// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt: runs a statically recompiled Switch game on its own small runtime,
// without an emulator.
//
//   nxrt --game <dir>
//
// <dir> holds the game's installed files: exefs/ (main.npdm, rtld, main, sdk...),
// exefs/romfs.bin and control/.
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "version.h"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <filesystem>
#include <string>

#include "cpu.h"
#include "frontend/window.h"
#include "hle/ipc.h"
#include "kernel/kernel.h"
#include "kernel/svc.h"
#include "loader.h"
#include "nvn/nvn_layer.h"
#include "nvn/recorder.h"
#include "render/nvn_renderer.h"
#include "render/serial_renderer_queue.h"
#include "replay/capturing_renderer.h"
#include "memory.h"
#include "runtime.h"
#include "tts_trace.h"
#include "guest_sampler.h"

namespace fs = std::filesystem;
using namespace nxrt;

namespace nxrt {
RuntimeConfig g_config;
}

static void OnExit() {
    if (std::getenv("NXRT_SVC_STATS")) DumpSvcStats();
    if (std::getenv("NXRT_IPC_STATS")) hle::DumpIpcStats();
    if (std::getenv("NXRT_NVN_STATS")) nvn::DumpStats();
}

static void OnSignal(int sig) {
    OnExit();
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

int main(int argc, char** argv) {
    LOG_INFO("nxrt " NXRT_VERSION);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--game" && i + 1 < argc) {
            g_config.game_dir = argv[++i];
        } else if (a == "--headless") {
            if (g_config.offscreen) {
                std::fprintf(stderr, "--headless and --offscreen are mutually exclusive\n");
                return 2;
            }
            g_config.headless = true;
        } else if (a == "--offscreen") {
            if (g_config.headless) {
                std::fprintf(stderr, "--headless and --offscreen are mutually exclusive\n");
                return 2;
            }
            g_config.offscreen = true;
        } else if (a == "--user" && i + 1 < argc) {
            g_config.user_dir = argv[++i];
        } else {
            std::fprintf(stderr, "usage: %s --game <installed game dir> [--user <save dir>] [--headless|--offscreen]\n",
                         argv[0]);
            return 2;
        }
    }
    if (g_config.game_dir.empty()) {
        std::fprintf(stderr, "usage: %s --game <installed game dir> [--user <save dir>]\n", argv[0]);
        return 2;
    }
    if (g_config.user_dir.empty()) g_config.user_dir = g_config.game_dir / "user";
    // Fonts next to the executable, else the ones in the source tree.
#ifdef _WIN32
    wchar_t exe_path[32768];
    const DWORD exe_len = GetModuleFileNameW(nullptr, exe_path, 32768);
    const fs::path exe_dir = exe_len ? fs::path(exe_path).parent_path() : fs::path();
    g_config.font_dir = exe_dir / "fonts";
#else
    g_config.font_dir = fs::canonical("/proc/self/exe").parent_path() / "fonts";
#endif
    if (!fs::exists(g_config.font_dir)) g_config.font_dir = NXRT_SOURCE_FONT_DIR;
    std::atexit(OnExit);
#ifdef _WIN32
    // No SIGUSR1 on Windows: NXRT_DUMP_THREADS_MS=<n> prints every guest thread
    // and what it is waiting for every n milliseconds.
    if (const char* ms = std::getenv("NXRT_DUMP_THREADS_MS")) {
        const long period = std::strtol(ms, nullptr, 10);
        if (period > 0) {
            std::thread([period] {
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(period));
                    DumpThreads();
                }
            }).detach();
        }
    }
#else
    // SIGUSR1 prints every guest thread and what it is waiting for.
    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &usr1, nullptr);
    std::thread([usr1] {
        for (;;) {
            int sig;
            if (sigwait(&usr1, &sig) == 0) DumpThreads();
        }
    }).detach();
#endif
    guest_sampler::Start(); // NXRT_GUEST_SAMPLE=<file> only
    tts_trace::Start(); // NXRT_TRACE_TTS=1 only (after the SIGUSR1 mask: its thread must inherit it)
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    const fs::path exefs = g_config.game_dir / "exefs";
    ProgramMetadata meta;
    if (!LoadNpdm(exefs / "main.npdm", meta)) {
        LOG_ERROR("could not read %s", (exefs / "main.npdm").string().c_str());
        return 1;
    }
    g_kernel.program_id = meta.program_id;
    LOG_INFO("program %016llx, main thread priority %u, stack 0x%x",
             static_cast<unsigned long long>(meta.program_id), meta.main_thread_priority,
             meta.main_thread_stack_size);

    if (!g_memory.Init()) return 1;
    syncstat::Init();
    std::vector<LoadedModule> modules;
    if (!LoadExeFs(exefs, AddressSpaceLayout::kCodeBase, modules)) {
        LOG_ERROR("could not load the game's modules from %s", exefs.string().c_str());
        return 1;
    }
    if (!InitRecompModules(modules)) return 1;
    nvn::InstallRecorder();
    nvn::InstallLayer(modules);

    hle::RegisterAllServices();

    // Main thread: stack in the stack region, entry at the first module (rtld),
    // x0 = 0 and x1 = its own handle, as the kernel starts an application.
    const u64 stack_size = (meta.main_thread_stack_size + kPageSize - 1) & ~(kPageSize - 1);
    const u64 stack_bottom = AddressSpaceLayout::kStackStart;
    g_memory.Map(stack_bottom, stack_size, MemState::Stack, PermRW);
    std::shared_ptr<KThread> main_thread;
    {
        std::lock_guard lock(g_kernel.lock);
        main_thread = CreateThread(modules.front().base, 0, stack_bottom + stack_size,
                                   meta.main_thread_priority, meta.main_thread_core);
        main_thread->ctx.x[1] = main_thread->handle;
    }
    if (g_config.headless && !g_config.offscreen) {
        LOG_INFO("starting the game (headless)");
        main_thread->started = true;
        RunGuest(main_thread.get());
        LOG_INFO("main thread exited");
        return 0;
    }
    if (!g_config.offscreen && !frontend::Init("Tomodachi Life (nxrt " NXRT_VERSION ")")) return 1;
    static std::unique_ptr<nvn::Renderer> renderer;
    static std::unique_ptr<replay::CapturingRenderer> capture_renderer;
    static std::unique_ptr<render::SerialRendererQueue> serial_renderer;
    const char* render_worker = std::getenv("NXRT_RENDER_WORKER");
    const char* execution_journal = std::getenv("NXRT_EXECUTION_MODEL_JOURNAL");
    const bool use_render_worker = render_worker && std::string(render_worker) == "1";
    const bool use_execution_journal = execution_journal && std::string(execution_journal) == "1";
    if (use_render_worker && use_execution_journal) {
        LOG_ERROR("NXRT_RENDER_WORKER cannot be combined with NXRT_EXECUTION_MODEL_JOURNAL");
        return 1;
    }
    if (!std::getenv("NXRT_NO_RENDER")) {
        renderer = render::CreateNvnRenderer(g_config.offscreen ? nullptr : frontend::Window());
        if (renderer) {
            nvn::Renderer* active_renderer = renderer.get();
            if (auto capture = replay::CaptureOptionsFromEnvironment(argv[0])) {
                capture_renderer = std::make_unique<replay::CapturingRenderer>(*renderer, std::move(*capture));
                active_renderer = capture_renderer.get();
            }
            if (use_render_worker) {
                serial_renderer = std::make_unique<render::SerialRendererQueue>(*active_renderer);
                nvn::SetRenderer(serial_renderer.get());
                LOG_INFO("renderer: synchronous serial worker enabled (NXRT_RENDER_WORKER=1)");
            } else {
                nvn::SetRenderer(active_renderer);
            }
        }
        else LOG_WARN("no Vulkan renderer: running without a picture");
    }
    if (g_config.offscreen) {
        LOG_INFO("starting the game with the offscreen Vulkan renderer");
        main_thread->started = true;
        RunGuest(main_thread.get());
        LOG_INFO("main thread exited");
        return 0;
    }
    LOG_INFO("starting the game");
    StartThread(main_thread);
    frontend::RunEventLoop();
}
