// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "nvn/model.h"

#include <utility>

namespace nxrt::nvn {

// Captures the command-list closure and deferred texture handles at QueueSubmit time.
// The caller must keep the Model lock while FindList resolves child handles. This does
// not snapshot guest buffers or their backing memory; it only owns the submitted commands.
template<class FindList, class ReadGuestHandle>
SubmissionSnapshot SnapshotSubmission(u64 root_handle, const CommandList& root, FindList&& find_list,
                                      ReadGuestHandle&& read_guest_handle) {
    SubmissionSnapshot snapshot;
    auto append = [&](auto&& self, u64 list_handle, const CommandList& list, u32 depth) -> void {
        for (const Command& source : list.commands) {
            if (source.op == Op::CallCommands) {
                if (depth >= 4) continue;
                for (size_t i = 0; i + 1 < source.words.size(); i += 2) {
                    const u64 child_handle = source.words[i] | (u64{source.words[i + 1]} << 32);
                    if (const CommandList* child = find_list(child_handle)) {
                        self(self, child_handle, *child, depth + 1);
                    }
                }
                continue;
            }

            Command command = source;
            if (source.op == Op::BindTexturesDeferred) {
                command.a[4] = 1;  // Snapshot marker; ApplyTextureBinding must not reread the guest pointer.
                command.a[5] = 0;  // Invalid until the entire range has been read successfully.
                command.words.clear();
                if (ValidTextureBindingRange(source.a[0], source.a[1], source.a[2])) {
                    if (source.a[2] == 0) {
                        command.a[5] = 1;
                    } else if (source.a[3] && source.a[3] <= ~u64{0} - 8 * (source.a[2] - 1)) {
                        std::array<u64, 32> handles{};
                        bool valid = true;
                        for (u64 i = 0; i < source.a[2]; ++i) {
                            const std::optional<u64> handle = read_guest_handle(source.a[3] + 8 * i);
                            if (!handle) {
                                valid = false;
                                break;
                            }
                            handles[i] = *handle;
                        }
                        if (valid) {
                            command.words.reserve(static_cast<size_t>(source.a[2] * 2));
                            for (u64 i = 0; i < source.a[2]; ++i) {
                                command.words.push_back(static_cast<u32>(handles[i]));
                                command.words.push_back(static_cast<u32>(handles[i] >> 32));
                            }
                            command.a[5] = 1;
                        }
                    }
                }
            }
            snapshot.list.commands.push_back(std::move(command));
            snapshot.list.origins.push_back({list_handle, depth});
        }
    };
    append(append, root_handle, root, 0);
    return snapshot;
}

} // namespace nxrt::nvn
