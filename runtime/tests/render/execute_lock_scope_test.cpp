#include "render/execute_lock_scope.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

using nxrt::render::ExecuteLockScope;
using nxrt::render::ExecuteModelRelease;
using namespace std::chrono_literals;

namespace {
void Require(bool condition) {
    if (!condition) std::abort();
}

template <class Future>
void Wait(Future& event) {
    // The deadline detects deadlock; lock assertions below are made with try_lock.
    Require(event.wait_for(5s) == std::future_status::ready);
}

void CheckFinalWait(bool enabled, bool pending, ExecuteModelRelease expected, bool has_snapshot = false) {
    std::mutex renderer_mutex, model_mutex;
    int model_version = 0;
    std::promise<void> executing, allow_final, waiting, allow_completion, recorder_probed, submit_probed;
    std::promise<void> submit_entered;
    auto executing_event = executing.get_future();
    auto allow_final_event = allow_final.get_future();
    auto waiting_event = waiting.get_future().share();
    auto allow_completion_event = allow_completion.get_future();
    auto recorder_probed_event = recorder_probed.get_future();
    auto submit_probed_event = submit_probed.get_future();
    auto submit_entered_event = submit_entered.get_future();

    std::thread first_submit([&] {
        ExecuteLockScope locks(renderer_mutex, model_mutex);
        executing.set_value();
        Wait(allow_final_event);  // represents ExecuteList; both locks must still be held
        Require(locks.BeforeFinalFlush(enabled, pending, has_snapshot) == expected);
        waiting.set_value();
        Wait(allow_completion_event);  // represents final GPU wait, with no later model access
    });

    Wait(executing_event);
    {
        std::unique_lock renderer_probe(renderer_mutex, std::try_to_lock);
        std::unique_lock model_probe(model_mutex, std::try_to_lock);
        Require(!renderer_probe.owns_lock() && !model_probe.owns_lock());
    }
    allow_final.set_value();
    Wait(waiting_event);

    std::thread recorder([&] {
        if (expected == ExecuteModelRelease::Released) {
            std::lock_guard model_lock(model_mutex);
            ++model_version;
            recorder_probed.set_value();
            return;
        }
        std::unique_lock model_probe(model_mutex, std::try_to_lock);
        Require(!model_probe.owns_lock());
        recorder_probed.set_value();
        model_probe.lock();  // disabled/pending fallback becomes available after Execute ends
        ++model_version;
    });
    std::thread second_submit([&] {
        {
            std::unique_lock renderer_probe(renderer_mutex, std::try_to_lock);
            Require(!renderer_probe.owns_lock());
        }
        submit_probed.set_value();
        ExecuteLockScope locks(renderer_mutex, model_mutex);
        submit_entered.set_value();
        model_version += 10;
    });

    Wait(recorder_probed_event);
    Wait(submit_probed_event);
    Require(submit_entered_event.wait_for(0s) == std::future_status::timeout);
    allow_completion.set_value();
    Wait(submit_entered_event);
    first_submit.join();
    recorder.join();
    second_submit.join();
    Require(model_version == 11);
    std::unique_lock renderer_probe(renderer_mutex, std::try_to_lock);
    std::unique_lock model_probe(model_mutex, std::try_to_lock);
    Require(renderer_probe.owns_lock() && model_probe.owns_lock());
}

void CheckException(bool enabled, bool pending, bool has_snapshot = false) {
    std::mutex renderer_mutex, model_mutex;
    try {
        ExecuteLockScope locks(renderer_mutex, model_mutex);
        locks.BeforeFinalFlush(enabled, pending, has_snapshot);
        throw std::runtime_error("simulated final flush failure");
    } catch (const std::runtime_error&) {
    }
    std::unique_lock renderer_probe(renderer_mutex, std::try_to_lock);
    std::unique_lock model_probe(model_mutex, std::try_to_lock);
    Require(renderer_probe.owns_lock() && model_probe.owns_lock());
}
} // namespace

int main() {
    CheckException(true, false, true);
    CheckFinalWait(true, false, ExecuteModelRelease::Disabled, true);
    CheckFinalWait(true, true, ExecuteModelRelease::Disabled, true);
    CheckFinalWait(false, false, ExecuteModelRelease::Disabled);
    CheckFinalWait(false, true, ExecuteModelRelease::Disabled);
    CheckFinalWait(true, false, ExecuteModelRelease::Released);
    CheckFinalWait(true, true, ExecuteModelRelease::PendingWritebacks);
    CheckException(false, false);
    CheckException(true, false);
    CheckException(true, true);
}
