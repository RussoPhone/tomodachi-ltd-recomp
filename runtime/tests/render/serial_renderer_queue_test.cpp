#include "nvn/recorder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "render/serial_renderer_queue.h"

using namespace std::chrono_literals;
using nxrt::nvn::CommandList;
using nxrt::nvn::Renderer;
using nxrt::nvn::SubmissionSnapshot;

namespace nxrt::render {
struct SerialRendererQueueTestAccess {
    static bool WaitForQueued(SerialRendererQueue& queue, size_t count) {
        std::unique_lock lock(queue.mutex_);
        return queue.cv_.wait_for(lock, 5s, [&] { return queue.jobs_.size() >= count; });
    }
};
} // namespace nxrt::render

namespace {
void Require(bool condition) {
    if (!condition) std::abort();
}

template <class Future>
void Wait(Future& future) {
    Require(future.wait_for(5s) == std::future_status::ready);
}

class FakeRenderer final : public Renderer {
public:
    void Execute(const CommandList& list) override { Record("execute:" + std::to_string(list.commands.size())); }
    void ExecuteSubmission(const SubmissionSnapshot& snapshot) override {
        if (snapshot.execution_model) {
            std::lock_guard lock(mutex);
            saw_expected_model = snapshot.execution_model.get() == expected_model;
        }
        Record("submit:" + std::to_string(snapshot.list.commands.size()));
    }
    void Present(nxrt::nvn::GuestAddr, u64 frame) override { Record("present:" + std::to_string(frame)); }

    void BlockNext() {
        std::lock_guard lock(mutex);
        block_next = true;
    }

    void WaitEntered() {
        std::unique_lock lock(mutex);
        Require(cv.wait_for(lock, 5s, [&] { return entered; }));
    }

    void Release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }

    std::vector<std::string> Calls() {
        std::lock_guard lock(mutex);
        return calls;
    }

    size_t MaxActive() const { return max_active.load(); }
    std::thread::id WorkerId() const {
        std::lock_guard lock(mutex);
        return worker_id;
    }

    void SetExpectedModel(const nxrt::nvn::Model* expected) {
        std::lock_guard lock(mutex);
        expected_model = expected;
    }

    bool SawExpectedModel() const {
        std::lock_guard lock(mutex);
        return saw_expected_model;
    }

    void SetThrowOnNext() {
        std::lock_guard lock(mutex);
        throw_next = true;
    }

private:
    void Record(std::string label) {
        const size_t active_now = active.fetch_add(1) + 1;
        size_t observed = max_active.load();
        while (observed < active_now && !max_active.compare_exchange_weak(observed, active_now)) {}
        std::unique_lock lock(mutex);
        if (worker_id == std::thread::id{}) worker_id = std::this_thread::get_id();
        Require(worker_id == std::this_thread::get_id());
        calls.push_back(std::move(label));
        if (throw_next) {
            throw_next = false;
            lock.unlock();
            active.fetch_sub(1);
            throw std::runtime_error("fake renderer failure");
        }
        if (block_next) {
            block_next = false;
            entered = true;
            cv.notify_all();
            Require(cv.wait_for(lock, 5s, [&] { return released; }));
        }
        lock.unlock();
        active.fetch_sub(1);
    }

private:
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::string> calls;
    std::thread::id worker_id{};
    const nxrt::nvn::Model* expected_model = nullptr;
    bool saw_expected_model = false;
    std::atomic<size_t> active{0};
    std::atomic<size_t> max_active{0};
    bool block_next = false;
    bool throw_next = false;
    bool entered = false;
    bool released = false;
};

void CheckSynchronousCompletionAndOrder() {
    auto fake = std::make_unique<FakeRenderer>();
    FakeRenderer* observed = fake.get();
    fake->BlockNext();
    nxrt::render::SerialRendererQueue queue(*fake);
    CommandList list;
    list.commands.emplace_back();
    std::promise<void> returned;
    auto returned_event = returned.get_future();
    std::thread caller([&] {
        queue.Execute(list);
        returned.set_value();
    });
    observed->WaitEntered();
    Require(returned_event.wait_for(20ms) == std::future_status::timeout);
    observed->Release();
    Wait(returned_event);
    caller.join();
    queue.Present(0, 7);
    queue.ExecuteSubmission({});
    const auto calls = observed->Calls();
    Require(calls == std::vector<std::string>{"execute:1", "present:7", "submit:0"});
    Require(observed->MaxActive() == 1);
    Require(observed->WorkerId() != std::this_thread::get_id());
}

void CheckSnapshotLifetimeAndExceptionDelivery() {
    auto fake = std::make_unique<FakeRenderer>();
    FakeRenderer* observed = fake.get();
    fake->BlockNext();
    nxrt::render::SerialRendererQueue queue(*fake);
    std::thread blocker([&] { queue.Execute({}); });
    observed->WaitEntered();
    SubmissionSnapshot snapshot;
    snapshot.list.commands.emplace_back();
    snapshot.execution_model = std::make_shared<nxrt::nvn::Model>();
    std::weak_ptr<nxrt::nvn::Model> retained_model = snapshot.execution_model;
    observed->SetExpectedModel(snapshot.execution_model.get());
    std::promise<void> submit_returned;
    auto submit_event = submit_returned.get_future();
    std::thread submitter([&] {
        queue.ExecuteSubmission(snapshot);
        submit_returned.set_value();
    });
    Require(nxrt::render::SerialRendererQueueTestAccess::WaitForQueued(queue, 1));
    snapshot.list.commands.clear();
    snapshot.execution_model.reset();
    Require(!retained_model.expired());
    observed->Release();
    Wait(submit_event);
    blocker.join();
    submitter.join();
    Require(observed->Calls() == std::vector<std::string>{"execute:0", "submit:1"});
    Require(observed->SawExpectedModel());

    observed->SetThrowOnNext();
    bool received = false;
    try {
        queue.Present(0, 9);
    } catch (const std::runtime_error&) {
        received = true;
    }
    Require(received);
    queue.Present(0, 10);
    Require(observed->Calls().back() == "present:10");
    Require(observed->MaxActive() == 1);
}

void CheckShutdownDrainAndReject() {
    auto fake = std::make_unique<FakeRenderer>();
    FakeRenderer* observed = fake.get();
    fake->BlockNext();
    nxrt::render::SerialRendererQueue queue(*fake);
    std::promise<void> first_returned;
    auto first_event = first_returned.get_future();
    std::thread first([&] {
        queue.Execute({});
        first_returned.set_value();
    });
    observed->WaitEntered();

    std::promise<void> second_started;
    auto second_started_event = second_started.get_future();
    std::promise<void> second_returned;
    auto second_event = second_returned.get_future();
    std::thread second([&] {
        second_started.set_value();
        try {
            queue.Present(0, 11);
            second_returned.set_value();
        } catch (...) {
            second_returned.set_exception(std::current_exception());
        }
    });
    Wait(second_started_event);
    Require(nxrt::render::SerialRendererQueueTestAccess::WaitForQueued(queue, 1));
    Require(second_event.wait_for(0ms) == std::future_status::timeout);
    std::thread shutdown([&] { queue.Shutdown(); });
    observed->Release();
    Wait(first_event);
    Wait(second_event);
    second_event.get();
    first.join();
    second.join();
    shutdown.join();
    Require(observed->Calls() == std::vector<std::string>{"execute:0", "present:11"});
    bool rejected = false;
    try {
        queue.Present(0, 12);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    Require(rejected);
}
} // namespace

int main() {
    CheckSynchronousCompletionAndOrder();
    CheckSnapshotLifetimeAndExceptionDelivery();
    CheckShutdownDrainAndReject();
}
