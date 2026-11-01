# Synchronous render-model journal (B3 slice)

`NXRT_EXECUTION_MODEL_JOURNAL=1` enables the opt-in B3 experiment described in the parent workspace's `orq/ASYNC-RENDER-DESENHO.md` §3.3. If `NXRT_EXECUTION_MODEL_SNAPSHOT=1` is also set, the journal takes precedence. With neither option enabled, the existing live-model path is selected.

The first submission creates a private model. Recorder hooks append value-owned changes under the live-model lock; each subsequent submission takes its changes, GPU pool addresses and sequence at its submit boundary. The renderer applies batches in captured order before executing the corresponding flattened command lists. The submit ordering mutex is held until synchronous ExecuteSubmission returns, so another submit cannot mutate the private model during execution. Recorder hooks can continue to append future changes. If a batch throws while being applied, the journal is marked unusable instead of silently processing a partially applied model.

The private Model can be reused and modified after ExecuteSubmission returns: it is not a general immutable object that an arbitrary renderer consumer may retain. The existing capture wrapper serializes state and forwards the submission before returning. GPU pool addresses are captured for each submit; changes increment a revision. The renderer invalidates address/texture-resolution caches when switching model lifetimes or when that revision changes. A distinct lifetime identity handles allocator address reuse.

This is still synchronous and holds the live model lock during render. It does not own or pin guest bytes/backings, delay fences, enable a render worker, or establish full asynchronous correctness. The future worker needs a single ordered queue for journal, executes, GPFIFO effects and present, plus backing lifetime and fence semantics.

Tests exercise real recorder hooks, texture/view reuse, programs, samplers, windows, pool finalize/remap, later GPU-address resolution, a blocked renderer with a second captured submission followed by a third mutation, cache model switching and allocator address reuse. Full-route visual and performance gates are separate from these tests.
