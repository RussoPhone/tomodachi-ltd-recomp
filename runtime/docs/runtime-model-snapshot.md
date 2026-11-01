# Submit-time execution metadata snapshot

`NXRT_EXECUTION_MODEL_SNAPSHOT=1` enables an experimental synchronous prerequisite for a future render worker. Unset or `0` keeps the live-model path.

QueueSubmit captures render-visible metadata under the recorder Model lock, once per submission. Flattened command lists in the same submission share that capture. Pools (including lazy GPU addresses), virtual mappings, textures, programs and stage vectors, texture/sampler descriptor tables and views, and windows own independent values. Recording/builders and finished lists are excluded: command closure and deferred texture handles already belong to SubmissionSnapshot.

The Vulkan renderer selects the captured metadata only while holding its own mutex, restores selection on every return/exception, and retains the live Model lock through the final flush. Model-lock yield and early tail release are suppressed while a snapshot is selected. Present and direct replay Execute calls continue using the live model. The capture wrapper forwards the exact submission and serializes the selected metadata into execution state events.

This captures metadata, not UBO/SSBO/vertex/index/texture bytes or memory backing lifetimes. It creates no render worker, delays no fence, and changes no compute writeback policy. Do not use it as evidence that early lock release or asynchronous execution is safe. Its allocation/copy cost needs an RTX A/B measurement before choosing a snapshot or journal strategy for the next phase.

Validation: focused metadata, recorder, active/inactive capture forwarding and concurrent lock tests; full build and CTest 42/42 on Linux. Tests mutate live shader stages/textures/views/mappings/windows after capture, check the next submission, verify one shared snapshot per QueueSubmit, and exercise early returns/exceptions and competing recorder/submit threads. Windows and full-route visual/performance gates remain unverified.

Design references in the parent workspace: orq/ASYNC-RENDER-DESENHO.md and orq/PERF60-B1-LOCK-REVISAO-CODEX.md. The latter rejects value-difference masking as a general compute write-set policy; this feature adds no such mask.

## Full raw gate (2026-10-26)

A/B/A with the same binary, snapshot 0/1/0, fixed initial game time and LEGACY60 on both sides reached q30780 with unchanged disposable saves. Per-frame q28080–30780 (2701 samples/run): p50 33.71 / 55.18 / 32.87 ms; p95 40.16 / 65.45 / 38.78 ms. Do not promote the full-copy implementation. GPU memory clocks also differed (median 6001 / 810 / 6001 MHz), so the observed slowdown does not isolate CPU snapshot duration. Text ROI differences stayed within control-to-control variation; complete raster equivalence is unproven. No faithful pacing gate or async worker was enabled.

Before proceeding: measure metadata capture time and map sizes separately, then evaluate immutable shared versions or a mutation journal while preserving submit-time ownership. Parent-workspace evidence: orq/logs/N1/PERF30-RUNTIME-SNAPSHOT/GATE-RAW-20261026.md. Source was unchanged during these runs; this update records the failed performance gate.
