// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include "memory.h"
#include "nvn/nvn_layer.h"
#include "nvn/recorder.h"

namespace {
struct Observers { nxrt::nvn::Observer call, returned; };
std::map<std::string, Observers> observers;
int failures = 0;
void Check(bool condition, const char* message) { if (!condition) { std::printf("FAIL: %s\n", message); ++failures; } }
void Invoke(const char* name, std::initializer_list<u64> args, u64 result = 0) {
    auto it = observers.find(name);
    if (it == observers.end()) { Check(false, name); return; }
    nxrt::nvn::Call call{};
    size_t i = 0; for (u64 arg : args) call.x[i++] = arg;
    if (it->second.call) it->second.call(call);
    call.ret = result;
    if (it->second.returned) it->second.returned(call);
}
}
namespace nxrt::nvn {
void Observe(const std::string& name, Observer call, Observer returned) { observers[name] = {std::move(call), std::move(returned)}; }
void SetCommandBufferHooks(std::function<void(u64, const std::string&)>, std::function<void(u64)>) {}
float Call::F(int i) const { float value; const u32 bits = static_cast<u32>(v[i]); std::memcpy(&value, &bits, 4); return value; }
}
namespace {
struct Values { bool valid = false; u32 width = 0, view = 0, view_object = 0, sampler_wrap = 0; u64 shader = 0, sampler = 0, window_texture = 0, virtual_gpu = 0, pool_revision = 0; bool pool = false, mapping = false; };
Values Read(const nxrt::nvn::Model* m) {
    if (!m) return {};
    Values v; v.valid = true;
    if (auto it = m->textures.find(0x55); it != m->textures.end()) v.width = it->second.desc.width;
    if (auto it = m->texture_pool_views.find(0x77); it != m->texture_pool_views.end() && it->second.count(1)) v.view = it->second.at(1).base_level;
    if (auto it = m->texture_views.find(0x66); it != m->texture_views.end()) v.view_object = it->second.base_level;
    if (auto it = m->programs.find(0x88); it != m->programs.end() && !it->second.stages.empty()) v.shader = it->second.stages[0].code;
    if (auto it = m->samplers.find(0x99); it != m->samplers.end()) v.sampler_wrap = it->second.wrap[0];
    if (auto it = m->sampler_pools.find(0xaa); it != m->sampler_pools.end() && it->second.count(2)) v.sampler = it->second.at(2);
    if (auto it = m->windows.find(0xbb); it != m->windows.end() && !it->second.textures.empty()) v.window_texture = it->second.textures[0];
    if (auto it = m->pools.find(0x100600); it != m->pools.end()) v.virtual_gpu = it->second.gpu;
    v.pool_revision = m->pool_address_revision;
    v.pool = m->pools.count(0x100500) != 0;
    v.mapping = m->virtual_mappings.count(0x500000) != 0;
    return v;
}
class Probe final : public nxrt::nvn::Renderer {
public:
    std::mutex mutex; std::condition_variable cv; bool first_entered = false, release = false;
    Values first, after_live_mutation, second, third, fourth; int calls = 0;
    void Execute(const nxrt::nvn::CommandList&) override {}
    void ExecuteSubmission(const nxrt::nvn::SubmissionSnapshot& submission) override {
        std::unique_lock lock(mutex);
        if (++calls == 1) {
            first = Read(submission.execution_model.get()); first_entered = true; cv.notify_all();
            cv.wait(lock, [&] { return release; });
            after_live_mutation = Read(submission.execution_model.get());
        } else if (calls == 2) { second = Read(submission.execution_model.get()); cv.notify_all(); }
        else if (calls == 3) { third = Read(submission.execution_model.get()); cv.notify_all(); }
        else { fourth = Read(submission.execution_model.get()); cv.notify_all(); }
    }
    void Present(u64,u64) override {}
};
void Submit() { Invoke("nvnQueueSubmitCommands", {0x1234, 1, 0x100080}); }
}
int main() {
#if defined(_WIN32)
    _putenv_s("NXRT_EXECUTION_MODEL_JOURNAL", "1");
#else
    setenv("NXRT_EXECUTION_MODEL_JOURNAL", "1", 1);
#endif
    using namespace nxrt; using namespace nxrt::nvn;
    if (!g_memory.Init() || !g_memory.Map(0x100000, 0x2000, MemState::Normal, PermRW)) return 2;
    InstallRecorder();
    g_memory.Write<u64>(0x100518, 0x600000); g_memory.Write<u64>(0x100520, 0x610000);
    g_memory.Write<u64>(0x100618, 0); g_memory.Write<u64>(0x100620, 0x510000);
    Invoke("nvnMemoryPoolBuilderSetDefaults", {0x10});
    Invoke("nvnMemoryPoolBuilderSetStorage", {0x10, 0x100a00, 0x100});
    Invoke("nvnMemoryPoolInitialize", {0x100500, 0x10});
    Invoke("nvnMemoryPoolBuilderSetDefaults", {0x11});
    Invoke("nvnMemoryPoolBuilderSetStorage", {0x11, 0x100b00, 0x100});
    Invoke("nvnMemoryPoolInitialize", {0x100600, 0x11});
    g_memory.Write<u64>(0x100618, 0x500000); // driver resolves address after pool initialization
    g_memory.Write<u64>(0x100700, 0x100500); g_memory.Write<u64>(0x100708, 0);
    g_memory.Write<u64>(0x100710, 0); g_memory.Write<u64>(0x100718, 0x80);
    Invoke("nvnMemoryPoolMapVirtual", {0x100600, 1, 0x100700});
    Invoke("nvnTextureBuilderSetDefaults", {0x20}); Invoke("nvnTextureBuilderSetWidth", {0x20, 128});
    Invoke("nvnTextureInitialize", {0x55, 0x20});
    Invoke("nvnTextureViewSetDefaults", {0x66}); Invoke("nvnTextureViewSetLevels", {0x66, 1, 2});
    Invoke("nvnTexturePoolRegisterTexture", {0x77, 1, 0x55, 0x66});
    Invoke("nvnSamplerBuilderSetDefaults", {0x30});
    Invoke("nvnSamplerBuilderSetWrapMode", {0x30, 4, 5, 6});
    Invoke("nvnSamplerInitialize", {0x99, 0x30});
    Invoke("nvnSamplerPoolRegisterSampler", {0xaa, 2, 0x99});
    g_memory.Write<u64>(0x100900, 0x55);
    Invoke("nvnWindowBuilderSetTextures", {0x40, 1, 0x100900});
    Invoke("nvnWindowInitialize", {0xbb, 0x40});
    Invoke("nvnProgramInitialize", {0x88});
    g_memory.Write<u64>(0x100800, 0x1111); g_memory.Write<u64>(0x100808, 0x2222);
    Invoke("nvnProgramSetShaders", {0x88, 1, 0x100800});
    Invoke("nvnCommandBufferBeginRecording", {0x100100});
    Invoke("nvnCommandBufferDrawArrays", {0x100100, 4, 0, 1});
    Invoke("nvnCommandBufferEndRecording", {0x100100}, 0x200000);
    g_memory.Write<u64>(0x100080, 0x200000);
    Probe renderer; SetRenderer(&renderer);
    std::thread first(Submit);
    { std::unique_lock lock(renderer.mutex); renderer.cv.wait(lock, [&] { return renderer.first_entered; }); }
    Invoke("nvnTextureBuilderSetWidth", {0x20, 256}); Invoke("nvnTextureInitialize", {0x55, 0x20});
    Invoke("nvnTextureViewSetLevels", {0x66, 3, 4}); Invoke("nvnTexturePoolRegisterTexture", {0x77, 1, 0x55, 0x66});
    g_memory.Write<u64>(0x100800, 0x3333); Invoke("nvnProgramSetShaders", {0x88, 1, 0x100800});
    Invoke("nvnSamplerBuilderSetWrapMode", {0x30, 7, 8, 9});
    Invoke("nvnSamplerInitialize", {0x99, 0x30});
    Invoke("nvnSamplerPoolRegisterSampler", {0xaa, 2, 0x98});
    g_memory.Write<u64>(0x100900, 0x56);
    Invoke("nvnWindowBuilderSetTextures", {0x40, 1, 0x100900});
    Invoke("nvnWindowInitialize", {0xbb, 0x40});
    Invoke("nvnMemoryPoolFinalize", {0x100500});
    g_memory.Write<u64>(0x100618, 0x520000);
    std::promise<void> started; auto started_event = started.get_future();
    std::thread second([&] { started.set_value(); Submit(); }); started_event.wait();
    // The second submit must record its boundary while the first renderer call is
    // still blocked. A later mutation belongs only to the third submit.
    bool captured_second = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
        { std::lock_guard lock(GetModel().lock); captured_second = GetModel().frame.submits >= 2; }
        if (captured_second) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Check(captured_second, "second submit captures its model boundary before waiting for the first render");
    g_memory.Write<u64>(0x100618, 0x530000);
    Invoke("nvnTextureBuilderSetWidth", {0x20, 512}); Invoke("nvnTextureInitialize", {0x55, 0x20});
    { std::unique_lock lock(renderer.mutex); Check(!renderer.cv.wait_for(lock, std::chrono::milliseconds(30), [&] { return renderer.calls > 1; }),
        "second submission cannot apply newer changes while the first executes"); renderer.release = true; renderer.cv.notify_all(); }
    first.join(); second.join();
    Submit();
    Check(renderer.third.valid && renderer.third.width == 512,
          "mutation after second capture belongs to the third submission");
    Check(renderer.first.virtual_gpu == 0x500000 && renderer.after_live_mutation.virtual_gpu == 0x500000 &&
          renderer.second.virtual_gpu == 0x520000 && renderer.third.virtual_gpu == 0x530000 &&
          renderer.second.pool_revision > renderer.first.pool_revision && renderer.third.pool_revision > renderer.second.pool_revision,
          "late GPU addresses are captured per submit and invalidate resolution caches");
    Invoke("nvnTextureFinalize", {0x55});
    Invoke("nvnProgramInitialize", {0x88});
    Invoke("nvnMemoryPoolInitialize", {0x100500, 0x10});
    g_memory.Write<u64>(0x100618, 0x500000);
    Invoke("nvnMemoryPoolMapVirtual", {0x100600, 1, 0x100700});
    Submit(); SetRenderer(nullptr);
    Check(renderer.first.valid && renderer.first.width == 128 && renderer.first.view == 1 && renderer.first.view_object == 1 && renderer.first.shader == 0x1111 && renderer.first.pool && renderer.first.mapping && renderer.first.sampler_wrap == 4 && renderer.first.sampler == 0x99 && renderer.first.window_texture == 0x55,
          "first submission sees original texture, view, shader and physical/virtual pool");
    Check(renderer.after_live_mutation.valid && renderer.after_live_mutation.width == 128 && renderer.after_live_mutation.view == 1 && renderer.after_live_mutation.view_object == 1 && renderer.after_live_mutation.shader == 0x1111 && renderer.after_live_mutation.pool && renderer.after_live_mutation.mapping && renderer.after_live_mutation.sampler_wrap == 4 && renderer.after_live_mutation.sampler == 0x99 && renderer.after_live_mutation.window_texture == 0x55,
          "live mutations cannot change model during the in-flight submission");
    Check(renderer.second.valid && renderer.second.width == 256 && renderer.second.view == 3 && renderer.second.view_object == 3 && renderer.second.shader == 0x3333 && !renderer.second.pool && !renderer.second.mapping && renderer.second.sampler_wrap == 7 && renderer.second.sampler == 0x98 && renderer.second.window_texture == 0x56,
          "next submission sees ordered replacements and pool-finalize mapping removal");
    Check(renderer.fourth.valid && renderer.fourth.width == 0 && renderer.fourth.shader == 0 && renderer.fourth.pool && renderer.fourth.mapping,
          "later submit sees texture deletion, program reset and pool reinitialization");
    return failures ? 1 : 0;
}
