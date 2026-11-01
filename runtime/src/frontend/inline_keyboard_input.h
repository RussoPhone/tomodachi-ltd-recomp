// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace nxrt::frontend {

enum class InlineKeyboardInputEventType { Text, Backspace, Confirm, Cancel };

struct InlineKeyboardInputEvent {
    InlineKeyboardInputEventType type;
    std::u16string text;
};

// Bridges SDL's main-thread keyboard events to the applet service thread.
class InlineKeyboardInput {
public:
    std::uint64_t Begin() {
        std::lock_guard lock(mutex_);
        events_.clear();
        active_ = true;
        const std::uint64_t session = ++session_;
        cv_.notify_all();
        return session;
    }

    void End(std::uint64_t session) {
        std::lock_guard lock(mutex_);
        if (session != session_) return;
        active_ = false;
        events_.clear();
        ++session_;
        cv_.notify_all();
    }

    bool Active() const {
        std::lock_guard lock(mutex_);
        return active_;
    }

    void PushText(std::u16string text) {
        if (text.empty()) return;
        Push({InlineKeyboardInputEventType::Text, std::move(text)});
    }

    void PushKey(InlineKeyboardInputEventType type) {
        if (type == InlineKeyboardInputEventType::Text) return;
        Push({type, {}});
    }

    std::vector<InlineKeyboardInputEvent> Drain() {
        std::lock_guard lock(mutex_);
        std::vector<InlineKeyboardInputEvent> result;
        result.reserve(events_.size());
        while (!events_.empty()) {
            result.push_back(std::move(events_.front()));
            events_.pop_front();
        }
        return result;
    }

    std::vector<InlineKeyboardInputEvent> WaitAndDrain(std::uint64_t session) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this, session] {
            return session != session_ || !active_ || !events_.empty();
        });
        if (session != session_ || !active_) return {};
        std::vector<InlineKeyboardInputEvent> result;
        result.reserve(events_.size());
        while (!events_.empty()) {
            result.push_back(std::move(events_.front()));
            events_.pop_front();
        }
        return result;
    }

private:
    void Push(InlineKeyboardInputEvent event) {
        {
            std::lock_guard lock(mutex_);
            if (!active_) return;
            events_.push_back(std::move(event));
        }
        cv_.notify_one();
    }

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool active_ = false;
    std::uint64_t session_ = 0;
    std::deque<InlineKeyboardInputEvent> events_;
};

} // namespace nxrt::frontend
