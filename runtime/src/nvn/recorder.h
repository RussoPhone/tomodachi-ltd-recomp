// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "nvn/model.h"

namespace nxrt::nvn {

// Draws what the recorder sees. Execute runs a submitted command list; Present
// shows the window texture the game presented.
class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void Execute(const CommandList& list) = 0;
    virtual void ExecuteSubmission(const SubmissionSnapshot& snapshot) {
        Execute(snapshot.list);
    }
    virtual void Present(GuestAddr texture, u64 frame) = 0;
    // Make GPU texture writes in this pool range visible to subsequent CPU reads.
    virtual bool InvalidateMappedRange(GuestAddr, u64, u64) { return false; }
};

// Registers the observers that keep the Model current. Call before the game starts.
void InstallRecorder();
Model& GetModel();
void SetRenderer(Renderer* renderer);

} // namespace nxrt::nvn
