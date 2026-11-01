// SPDX-License-Identifier: GPL-3.0-or-later
// Library applets the game launches (software keyboard, error viewer, ...).
// Each applet is a small backend: it receives the game's input storages and
// answers with output storages. Applets without a backend finish immediately
// with no output, and the log names them.
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <chrono>
#include <thread>

#include "frontend/window.h"
#include "frontend/inline_keyboard_text.h"
#include "frontend/inline_keyboard_protocol.h"
#include "services/common.h"

namespace nxrt::hle {

namespace {

constexpr u32 kResultNoDataInChannel = MakeResult(128, 2);

bool TraceInlineKeyboard() {
    static const bool enabled = [] {
        const char* value = std::getenv("NXRT_TRACE_KEYBOARD");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

// Software keyboard (AppletId 0x11) output storage, as the game reads it:
// u64 result (0 = Ok, 1 = Cancel) followed by the UTF-16 text, NUL terminated.
constexpr u32 kAppletIdSwkbd = 0x11;
constexpr size_t kSwkbdOutputSize = 0x7D8;
constexpr size_t kSwkbdTextOffset = sizeof(u64);
constexpr size_t kSwkbdMaxChars = (kSwkbdOutputSize - kSwkbdTextOffset) / sizeof(char16_t) - 1;

constexpr std::array<u8, kSwkbdOutputSize> BuildSwkbdOutput(const char16_t* text, size_t length, u64 result) {
    std::array<u8, kSwkbdOutputSize> out{};
    for (size_t i = 0; i < 8; i++) out[i] = static_cast<u8>(result >> (8 * i));
    if (length > kSwkbdMaxChars) length = kSwkbdMaxChars;
    for (size_t i = 0; i < length; i++) {
        out[kSwkbdTextOffset + i * 2] = static_cast<u8>(text[i] & 0xFF);
        out[kSwkbdTextOffset + i * 2 + 1] = static_cast<u8>(text[i] >> 8);
    }
    return out;
}

// Static contract check: "Ok" + "Ab" lands as 0u64, 'A',0,'b',0,0,0.
constexpr auto kSwkbdSample = BuildSwkbdOutput(u"Ab", 2, 0);
static_assert(kSwkbdSample[0] == 0 && kSwkbdSample[7] == 0 && kSwkbdSample[8] == 'A' &&
              kSwkbdSample[9] == 0 && kSwkbdSample[10] == 'b' && kSwkbdSample[12] == 0 &&
              kSwkbdSample.size() == 0x7D8);
static_assert(BuildSwkbdOutput(u"x", 1, 1)[0] == 1);

// Inline keyboard (nn::swkbd::InlineKeyboard, the only keyboard API the game imports):
// the game pushes requests with PushInteractiveInData and polls PopInteractiveOutData.
// Layouts as in suyu's applet_software_keyboard_types.h; not yet validated with the game.
constexpr u32 kLibraryAppletModeAllForeground = 0;
constexpr u32 kReqFinalize = 0x4, kReqSetUserWordInfo = 0x6, kReqSetCustomizeDic = 0x7,
              kReqCalc = 0xA, kReqSetCustomizedDictionaries = 0xB,
              kReqUnsetCustomizedDictionaries = 0xC, kReqSetChangedStringV2 = 0xD,
              kReqSetMovedCursorV2 = 0xE;
constexpr u32 kReplyFinishedInitialize = 0x0, kReplyDefault = 0x1, kReplyChangedString = 0x2,
              kReplyDecidedEnter = 0x5,
              kReplyChangedStringUtf8 = 0x7, kReplyDecidedEnterUtf8 = 0x9,
              kReplyUnsetCustomizeDic = 0xA,
              kReplyReleasedUserWordInfo = 0xB, kReplyUnsetCustomizedDictionaries = 0xC,
              kReplyChangedStringV2 = 0xD, kReplyChangedStringUtf8V2 = 0xF;
constexpr u32 kStateNotInitialized = 0, kStateHidden = 1, kStateAppearing = 2, kStateShown = 3,
              kStateDisappearing = 4;
constexpr size_t kReplyBaseSize = 8;      // u32 state + u32 reply type
constexpr size_t kReplyUtf8Size = 0x7D4;  // text area of the UTF-8 replies
constexpr size_t kReplyUtf16Size = 0x3EC; // text area of the UTF-16 replies
constexpr size_t kCalcCommonSize = 0x18;
constexpr size_t kCalcOldTotal = 0x4A0, kCalcNewTotal = 0x4E8;
constexpr size_t kInputTextChars = 0x1FA;
constexpr u32 kInlineDefaultMaxChars = 500;
static_assert(0x78 + kInputTextChars * 2 < kCalcNewTotal - kCalcCommonSize &&
              0x50 + kInputTextChars * 2 < kCalcOldTotal - kCalcCommonSize);
constexpr u64 kFlagInitialize = 1ull << 0, kFlagAppear = 1ull << 2, kFlagInputText = 1ull << 3,
              kFlagCursor = 1ull << 4, kFlagUtf8 = 1ull << 5, kFlagUnsetCustomizeDic = 1ull << 6,
              kFlagDisappear = 1ull << 7, kFlagUnsetUserWordInfo = 1ull << 10;

// Text the keyboard "types": what was typed in the window (consumed), else
// NXRT_SWKBD_TEXT, else NXRT_USER_NAME, else `current` (text the game set), else "Nxrt".
std::u16string SwkbdText(const std::u16string& current = {}) {
    std::u16string typed = frontend::TakeTypedText();
    if (!typed.empty()) return typed;
    const char* e = std::getenv("NXRT_SWKBD_TEXT");
    if (!e || !*e) e = std::getenv("NXRT_USER_NAME");
    if (!e || !*e) {
        if (!current.empty()) return current;
        e = "Nxrt";
    }
    return frontend::inline_keyboard_text::Utf8ToUtf16(e);
}

std::u16string SwkbdPrefill(const std::u16string& current) {
    const char* e = std::getenv("NXRT_SWKBD_TEXT");
    if (!e || !*e) e = std::getenv("NXRT_USER_NAME");
    if (!e || !*e) return current;
    return frontend::inline_keyboard_text::Utf8ToUtf16(e);
}

class ILibraryAppletAccessor final : public ServiceObject {
public:
    ILibraryAppletAccessor(u32 applet_id, u32 mode)
        : ServiceObject("ILibraryAppletAccessor"), applet_id_(applet_id), mode_(mode),
          inline_(applet_id == kAppletIdSwkbd && mode != kLibraryAppletModeAllForeground) {
        Register(0, "GetAppletStateChangedEvent", [this](Ctx& ctx) { ctx.PushCopyObject(state_event_); });
        Register(1, "IsCompleted", [this](Ctx& ctx) { ctx.Push<u8>(completed_); });
        Register(10, "Start", [this](Ctx&) {
            if (inline_) {
                // Inline keyboard: stays alive until the game sends Finalize.
                LOG_INFO("inline swkbd started (mode %u, %zu input storages)", mode_, in_.size());
                return;
            }
            if (applet_id_ == kAppletIdSwkbd) {
                const std::u16string text = SwkbdText();
                const auto out = BuildSwkbdOutput(text.data(), text.size(), 0);
                out_.push_back(std::make_shared<IStorage>(std::vector<u8>(out.begin(), out.end())));
                LOG_INFO("swkbd started (mode %u, %zu input storages): answering Ok with %zu chars",
                         mode_, in_.size(), text.size());
                out_event_->Signal();
            } else {
                LOG_INFO("library applet 0x%x started (mode %u, %zu input storages); no backend yet, finishing",
                         applet_id_, mode_, in_.size());
            }
            completed_ = true;
            state_event_->Signal();
        });
        Register(20, "RequestExit", [this](Ctx&) {
            completed_ = true;
            state_event_->Signal();
        });
        RegisterStub(25, "Terminate");
        Register(30, "GetResult", [](Ctx&) {});
        RegisterStub(50, "SetOutOfFocusApplicationSuspendingEnabled");
        RegisterStub(60, "PresetLibraryAppletGpuTimeSliceZero");
        RegisterStub(90, "Unknown90");
        Register(100, "PushInData", [this](Ctx& ctx) { PushStorage(ctx, in_); });
        Register(101, "PopOutData", [this](Ctx& ctx) { PopStorage(ctx, out_); });
        Register(103, "PushInteractiveInData", [this](Ctx& ctx) {
            PushStorage(ctx, interactive_in_);
            if (!inline_) return;
            while (!interactive_in_.empty()) {
                const auto st = std::move(interactive_in_.front());
                interactive_in_.pop_front();
                if (ProcessInlineRequest(st->data)) StartInlineInputThread();
            }
        });
        Register(104, "PopInteractiveOutData", [this](Ctx& ctx) {
            if (!inline_) {
                PopStorage(ctx, interactive_out_);
                return;
            }
            std::lock_guard lock(inline_mutex_);
            PopStorage(ctx, interactive_out_);
        });
        Register(105, "GetPopOutDataEvent", [this](Ctx& ctx) { ctx.PushCopyObject(out_event_); });
        Register(106, "GetPopInteractiveOutDataEvent",
                 [this](Ctx& ctx) { ctx.PushCopyObject(interactive_out_event_); });
        Register(110, "NeedsToExitProcess", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(150, "RequestForAppletToGetForeground", [](Ctx&) {});
        Register(160, "GetIndirectLayerConsumerHandle", [](Ctx& ctx) { ctx.Push<u64>(1); });
    }

    ~ILibraryAppletAccessor() override {
        if (inline_input_session_) frontend::EndInlineKeyboardInput(inline_input_session_);
        if (inline_input_thread_.joinable()) inline_input_thread_.join();
    }

private:
    void PushStorage(Ctx& ctx, std::deque<std::shared_ptr<IStorage>>& queue) {
        auto st = ctx.in_objects.empty() ? nullptr : std::dynamic_pointer_cast<IStorage>(ctx.in_objects[0]);
        if (!st && !ctx.copy_handles.empty()) {
            std::lock_guard lock(g_kernel.lock);
            auto session = g_kernel.handles.GetAs<KClientSession>(ctx.copy_handles[0]);
            if (session) st = std::dynamic_pointer_cast<IStorage>(session->state->object);
        }
        if (st) queue.push_back(st);
    }
    // --- inline keyboard ---
    template <typename T>
    static T Rd(const std::vector<u8>& d, size_t off) {
        T v{};
        if (off + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + off, sizeof(T));
        return v;
    }

    void Reply(u32 type, size_t size, const std::u16string* text = nullptr, bool utf8 = false,
               bool v2 = false) {
        std::vector<u8> r(size);
        std::memcpy(r.data(), &swkbd_state_, 4);
        std::memcpy(r.data() + 4, &type, 4);
        if (text) {
            const size_t area = utf8 ? kReplyUtf8Size : kReplyUtf16Size;
            std::string u8s;
            if (utf8) {
                u8s = frontend::inline_keyboard_text::Utf16ToUtf8(*text);
                std::memcpy(r.data() + kReplyBaseSize, u8s.data(), std::min(u8s.size(), area - 4));
            } else {
                std::memcpy(r.data() + kReplyBaseSize, text->data(),
                            std::min(text->size() * 2, area - 4));
            }
            const u32 len = static_cast<u32>(text->size());
            const bool changed = type == kReplyChangedString || type == kReplyChangedStringUtf8 ||
                                 type == kReplyChangedStringV2 || type == kReplyChangedStringUtf8V2;
            u8* arg = r.data() + kReplyBaseSize + area;
            std::memcpy(arg, &len, 4);
            if (changed) { // dictionary start/end = -1, cursor
                const s32 m1 = -1;
                std::memcpy(arg + 4, &m1, 4);
                std::memcpy(arg + 8, &m1, 4);
                const s32 cur = cursor_;
                std::memcpy(arg + 12, &cur, 4);
            }
            (void)v2; // trailing V2 flag byte stays 0
        }
        interactive_out_.push_back(std::make_shared<IStorage>(std::move(r)));
        interactive_out_event_->Signal();
    }
    void ChangeState(u32 st) {
        swkbd_state_ = st;
        Reply(kReplyDefault, kReplyBaseSize);
    }

    void ReplyChangedString() {
        text_reported_ = true;
        const bool u8m = utf8_;
        if (TraceInlineKeyboard()) {
            LOG_INFO("inline swkbd trace: ChangedString chars=%zu cursor=%d utf8=%u", text_.size(),
                     cursor_, u8m ? 1u : 0u);
        }
        if (u8m) Reply(changed_v2_ ? kReplyChangedStringUtf8V2 : kReplyChangedStringUtf8,
                       kReplyBaseSize + kReplyUtf8Size + 0x10 + (changed_v2_ ? 1 : 0), &text_, true);
        else Reply(changed_v2_ ? kReplyChangedStringV2 : kReplyChangedString,
                   kReplyBaseSize + kReplyUtf16Size + 0x10 + (changed_v2_ ? 1 : 0), &text_, false);
    }

    void ReplyCancel() {
        const auto reply = frontend::inline_keyboard_protocol::BuildCancelReply(swkbd_state_);
        interactive_out_.push_back(std::make_shared<IStorage>(std::vector<u8>(reply.begin(), reply.end())));
        interactive_out_event_->Signal();
    }

    void FinishInput(bool confirm) {
        const bool u8m = utf8_;
        if (confirm) {
            Reply(u8m ? kReplyDecidedEnterUtf8 : kReplyDecidedEnter,
                  kReplyBaseSize + (u8m ? kReplyUtf8Size : kReplyUtf16Size) + 4, &text_, u8m);
            LOG_INFO("inline swkbd: confirmed %zu chars", text_.size());
        } else {
            ReplyCancel();
            LOG_INFO("inline swkbd: cancelled");
        }
        frontend::EndInlineKeyboardInput(inline_input_session_);
        ChangeState(kStateDisappearing);
        ChangeState(kStateHidden);
    }

    void HandleHostInput(const std::vector<frontend::InlineKeyboardInputEvent>& events) {
        bool changed = false;
        if (TraceInlineKeyboard() && !events.empty()) {
            LOG_INFO("inline swkbd trace: drained %zu host events", events.size());
        }
        for (const auto& event : events) {
            using frontend::InlineKeyboardInputEventType;
            switch (event.type) {
            case InlineKeyboardInputEventType::Text:
                {
                    const auto old_text = text_;
                    const auto old_cursor = cursor_;
                    frontend::inline_keyboard_text::Insert(text_, cursor_, event.text, max_chars_);
                    changed = changed || old_text != text_ || old_cursor != cursor_;
                    if (TraceInlineKeyboard()) {
                        LOG_INFO("inline swkbd trace: applied text chars=%zu cursor=%d", text_.size(), cursor_);
                    }
                }
                break;
            case InlineKeyboardInputEventType::Backspace:
                {
                    const auto old_text = text_;
                    const auto old_cursor = cursor_;
                    frontend::inline_keyboard_text::Backspace(text_, cursor_);
                    changed = changed || old_text != text_ || old_cursor != cursor_;
                    if (TraceInlineKeyboard()) {
                        LOG_INFO("inline swkbd trace: applied backspace chars=%zu cursor=%d", text_.size(), cursor_);
                    }
                }
                break;
            case InlineKeyboardInputEventType::Confirm:
                if (changed || (!text_reported_ && !text_.empty())) ReplyChangedString(); // prefilled text
                FinishInput(true);
                return;
            case InlineKeyboardInputEventType::Cancel:
                FinishInput(false);
                return;
            }
        }
        if (changed) ReplyChangedString();
    }

    void StartInlineInputThread() {
        if (inline_input_thread_.joinable()) inline_input_thread_.join();
        const u64 session = inline_input_session_;
        inline_input_thread_ = std::thread([this, session] {
            for (;;) {
                auto events = frontend::WaitInlineKeyboardInput(session);
                if (events.empty()) return;
                std::lock_guard lock(inline_mutex_);
                if (inline_input_session_ != session || swkbd_state_ != kStateShown) return;
                HandleHostInput(events);
                if (swkbd_state_ != kStateShown) return;
            }
        });
    }

    bool ProcessInlineRequest(const std::vector<u8>& d) {
        std::lock_guard lock(inline_mutex_);
        if (d.size() < 4) return false;
        const u32 cmd = Rd<u32>(d, 0);
        if (TraceInlineKeyboard() && swkbd_state_ == kStateShown) {
            static u32 shown_requests = 0;
            ++shown_requests;
            if (shown_requests <= 5 || shown_requests % 120 == 0) {
                LOG_INFO("inline swkbd trace: guest request #%u cmd=0x%x", shown_requests, cmd);
            }
        }
        switch (cmd) {
        case kReqFinalize:
            LOG_INFO("inline swkbd: Finalize");
            frontend::EndInlineKeyboardInput(inline_input_session_);
            swkbd_state_ = kStateNotInitialized;
            Reply(kReplyDefault, kReplyBaseSize);
            completed_ = true;
            state_event_->Signal();
            return false;
            break;
        case kReqSetUserWordInfo: Reply(kReplyReleasedUserWordInfo, kReplyBaseSize); break;
        case kReqUnsetCustomizedDictionaries:
            Reply(kReplyUnsetCustomizedDictionaries, kReplyBaseSize);
            break;
        case kReqSetCustomizeDic:
        case kReqSetCustomizedDictionaries: break; // not implemented in suyu either
        case kReqSetChangedStringV2: changed_v2_ = Rd<u8>(d, 4) != 0; break;
        case kReqSetMovedCursorV2: break;
        case kReqCalc: {
            if (d.size() < 4 + kCalcCommonSize) return false;
            const u16 calc_size = Rd<u16>(d, 4 + 4);
            const u64 flags = Rd<u64>(d, 4 + 8);
            const size_t body = 4 + kCalcCommonSize;
            const bool old_layout = calc_size == kCalcOldTotal;
            const size_t appear_off = body + (old_layout ? 8 : 0);
            const size_t cursor_off = body + (old_layout ? 4 : 0x74);
            const size_t text_off = body + (old_layout ? 0x50 : 0x78);
            const size_t utf8_off = text_off + kInputTextChars * 2;
            if (flags & kFlagInputText) {
                text_.clear();
                for (size_t i = 0; i < kInputTextChars; i++) {
                    const char16_t c = Rd<char16_t>(d, text_off + i * 2);
                    if (!c) break;
                    text_.push_back(c);
                }
            }
            if (flags & kFlagCursor)
                cursor_ = frontend::inline_keyboard_text::NormalizeCursor(text_, Rd<s32>(d, cursor_off));
            if (flags & kFlagUtf8) utf8_ = Rd<u8>(d, utf8_off) != 0;
            if (swkbd_state_ <= kStateHidden && (flags & kFlagUnsetCustomizeDic))
                Reply(kReplyUnsetCustomizeDic, kReplyBaseSize);
            if (swkbd_state_ <= kStateHidden && (flags & kFlagUnsetUserWordInfo))
                Reply(kReplyReleasedUserWordInfo, kReplyBaseSize);
            if (swkbd_state_ == kStateNotInitialized && (flags & kFlagInitialize)) {
                swkbd_state_ = kStateHidden;
                Reply(kReplyFinishedInitialize, kReplyBaseSize + 1);
                LOG_INFO("inline swkbd: initialized (calc size 0x%x, %s layout)", calc_size,
                         old_layout ? "old" : "new");
            }
            bool start_input_thread = false;
            if (swkbd_state_ == kStateHidden && (flags & kFlagAppear)) {
                const u32 mx = Rd<u32>(d, appear_off + 32);
                max_chars_ = (mx > 0 && mx <= kInlineDefaultMaxChars) ? mx : kInlineDefaultMaxChars;
                text_ = SwkbdPrefill(text_);
                text_reported_ = false;
                text_ = frontend::inline_keyboard_text::Truncate(text_, max_chars_);
                cursor_ = static_cast<s32>(text_.size());
                inline_input_session_ = frontend::BeginInlineKeyboardInput();
                ChangeState(kStateAppearing);
                ChangeState(kStateShown);
                start_input_thread = true;
                LOG_INFO("inline swkbd: waiting for host text or gamepad input");
                // Replays/headless runs have no host keyboard: NXRT_SWKBD_AUTOCONFIRM_MS=<ms> presses Enter
                // after that delay (with NXRT_SWKBD_TEXT as the typed name).
                if (const char* ac = std::getenv("NXRT_SWKBD_AUTOCONFIRM_MS"); ac && *ac) {
                    const long ms = std::strtol(ac, nullptr, 10);
                    const char* ty = std::getenv("NXRT_SWKBD_AUTOTYPE"); // type like a user: one char at a time
                    const std::u16string typed = ty && *ty ? frontend::inline_keyboard_text::Utf8ToUtf16(ty) : u"";
                    std::thread([ms, typed] {
                        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                        for (const char16_t c : typed) {
                            frontend::TypeInlineKeyboardText(std::u16string(1, c));
                            std::this_thread::sleep_for(std::chrono::milliseconds(150));
                        }
                        frontend::ConfirmInlineKeyboardInput();
                    }).detach();
                }
            } else if (swkbd_state_ == kStateShown && (flags & kFlagDisappear)) {
                frontend::EndInlineKeyboardInput(inline_input_session_);
                ChangeState(kStateDisappearing);
                ChangeState(kStateHidden);
            }
            return start_input_thread;
        }
        default:
            LOG_WARN("inline swkbd: unknown request 0x%x (%zu bytes), ignored", cmd, d.size());
            break;
        }
        return false;
    }

    void PopStorage(Ctx& ctx, std::deque<std::shared_ptr<IStorage>>& queue) {
        if (queue.empty()) {
            ctx.result = kResultNoDataInChannel;
            return;
        }
        ctx.PushInterface(queue.front());
        queue.pop_front();
    }

    u32 applet_id_, mode_;
    bool inline_;
    bool text_reported_ = false;
    std::mutex inline_mutex_;
    u64 inline_input_session_ = 0;
    std::thread inline_input_thread_;
    bool completed_ = false;
    u32 swkbd_state_ = kStateNotInitialized;
    std::u16string text_;
    s32 cursor_ = 0;
    u32 max_chars_ = kInlineDefaultMaxChars;
    bool utf8_ = false, changed_v2_ = false;
    std::shared_ptr<KEvent> state_event_ = MakeEvent("applet:state");
    std::shared_ptr<KEvent> out_event_ = MakeEvent("applet:out");
    std::shared_ptr<KEvent> interactive_out_event_ = MakeEvent("applet:interactive-out");
    std::deque<std::shared_ptr<IStorage>> in_, out_, interactive_in_, interactive_out_;
};

} // namespace

std::shared_ptr<ServiceObject> MakeLibraryAppletAccessor(u32 applet_id, u32 mode) {
    return std::make_shared<ILibraryAppletAccessor>(applet_id, mode);
}

} // namespace nxrt::hle
