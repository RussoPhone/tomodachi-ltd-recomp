// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "nvn/recorder.h"

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace nxrt::render {

struct SerialRendererQueueTestAccess;

// Serializes all renderer calls on one worker while preserving a synchronous
// completion contract for callers. The wrapped renderer must outlive this
// decorator; Shutdown() drains accepted calls before returning.
class SerialRendererQueue final : public nvn::Renderer {
public:
    explicit SerialRendererQueue(nvn::Renderer& renderer) : renderer_(renderer), worker_([this] { WorkerLoop(); }) {}

    ~SerialRendererQueue() override { Shutdown(); }

    SerialRendererQueue(const SerialRendererQueue&) = delete;
    SerialRendererQueue& operator=(const SerialRendererQueue&) = delete;

    void Execute(const nvn::CommandList& list) override {
        nvn::CommandList owned = list;
        RunSync([owned = std::move(owned)](nvn::Renderer& renderer) { renderer.Execute(owned); });
    }

    void ExecuteSubmission(const nvn::SubmissionSnapshot& snapshot) override {
        nvn::SubmissionSnapshot owned = snapshot;
        RunSync([owned = std::move(owned)](nvn::Renderer& renderer) { renderer.ExecuteSubmission(owned); });
    }

    void Present(nvn::GuestAddr texture, u64 frame) override {
        RunSync([texture, frame](nvn::Renderer& renderer) { renderer.Present(texture, frame); });
    }

    bool InvalidateMappedRange(nvn::GuestAddr pool, u64 offset, u64 size) override {
        bool changed = false;
        RunSync([pool, offset, size, &changed](nvn::Renderer& renderer) {
            changed = renderer.InvalidateMappedRange(pool, offset, size);
        });
        return changed;
    }

    void Shutdown() {
        std::lock_guard shutdown_lock(shutdown_mutex_);
        {
            std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) {
                throw std::logic_error("SerialRendererQueue cannot shut down from its worker");
            }
            worker_.join();
        }
    }

private:
    friend struct SerialRendererQueueTestAccess;

    void RunSync(std::function<void(nvn::Renderer&)> operation) {
        auto completion = std::make_shared<std::promise<void>>();
        auto result = completion->get_future();
        {
            std::lock_guard lock(mutex_);
            if (stopping_) throw std::logic_error("SerialRendererQueue is shut down");
            jobs_.push_back([this, operation = std::move(operation), completion] {
                try {
                    operation(renderer_);
                    completion->set_value();
                } catch (...) {
                    completion->set_exception(std::current_exception());
                }
            });
        }
        cv_.notify_all();
        result.get();
    }

    void WorkerLoop() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
                if (jobs_.empty()) {
                    if (stopping_) return;
                    continue;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }

    nvn::Renderer& renderer_;
    std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> jobs_;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace nxrt::render
