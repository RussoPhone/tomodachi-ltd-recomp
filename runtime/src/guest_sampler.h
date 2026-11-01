// SPDX-License-Identifier: GPL-3.0-or-later
// NXRT_GUEST_SAMPLE=<file>: opt-in diagnostic. A host thread snapshots the guest register file of the
// running guest threads every NXRT_GUEST_SAMPLE_US microseconds (default 2000) and, every 5 s, rewrites
// <file> with inclusive per-frame counts (how many samples had that guest function anywhere in the first
// 24 frames) for the thread ids in NXRT_GUEST_SAMPLE_TIDS (comma list, default: all). Reads are racy by
// design (no kernel lock); it never changes guest-visible behaviour. NXRT_GUEST_SAMPLE_FROM_S/TO_S limit
// the window in seconds since start.
#pragma once

namespace nxrt::guest_sampler {
void Start(); // no-op unless NXRT_GUEST_SAMPLE is set
}
