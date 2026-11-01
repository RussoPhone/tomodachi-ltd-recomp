# Roadmap: our own recompiler

🇧🇷 [Português](ROADMAP-RECOMPILER.pt-BR.md)

This is a **direction, not a schedule**. Priority one stays stabilizing what exists (installer, runtime, performance). The recompiler work
is a continuous process that moves one piece at a time, always with a working game.

## Why

Today the installer uses [mk8-recomp](https://github.com/dougchansan/mk8-recomp) (built on suyu code) to turn the game's ARM code into C.
That is the slowest and heaviest part of the installation, measured on a clean install (16-thread laptop, 16 GB RAM):

| | Measured |
|---|---|
| Generated C | ~4.5 GB in ~800 compilation units |
| Compiling it | 99 min wall time, ~780 min of CPU in total |
| Where the time goes | the 30 largest units are 55% of the CPU time; one 45 MB unit takes ~6 min alone and ~3.6 GB of RAM |
| Install, end to end | about 2 h 10 min and ~27 GB of disk with the first setting (15000 instructions per unit). Splitting into smaller units (6000) cut the compile step to 78 min and the peak RAM per compile from 3.5 GB to 1.6 GB, with the same total C, and is now the default |

Part of that size comes from the way the code is generated: every block carries a guard that re-checks the instruction words (to be able to
fall back to a JIT) and an entry dispatcher. The runtime (`nxrt`) has no JIT, so a recompiler of our own does not need either, and could
produce much less C. Replacing the recompiler would also remove suyu from the installer's tool chain.

## How the pieces fit today

- The generated code is plain C. Each guest block is `void blk_<module>_<address>(GuestContext*)`, and each module exports
  `recomp_image_lookup_<module>(address)`, which returns the function for a guest address.
- The contract between that code and the runtime is small: `GuestContext` (registers, flags, vectors, `pc`, a pending SVC number) and
  `RecompHostMem` (memory and exclusive-access callbacks). It lives in [`runtime/src/guest_abi.h`](../runtime/src/guest_abi.h) and
  [`runtime/src/cpu.cpp`](../runtime/src/cpu.cpp).

Because the runtime only asks "who executes this address?", **two recompilers can coexist**: ours answers for the addresses it covers and the
current one answers for the rest.

## Phases

| Phase | What | How we know it is right |
|---|---|---|
| 0. Stabilize (now) | Clean-install testing, issues, 1.7.x | Reports from `./install.sh --report` |
| 1. Own extraction tools | Read the `.nsp`, decrypt and extract the modules without suyu | The module hashes match the ones in `adapters/tomodachi/target.json` |
| 2. Recompiler v0 | Same block signature and same `GuestContext`; start with the small `rtld` module and simple functions; no JIT guards | A differential test against a reference CPU emulator, plus the island replay routes |
| 3. Growing coverage | The `sdk` module, then `main`, function by function | A report of "% of functions from our recompiler" |
| 4. 100% ours | mk8-recomp leaves the installer; the ABI may then change freely | The full game runs and the installer no longer downloads suyu |

## Rules for the transition

- **Always a working game.** Each step is checked with the game and the replay before moving on.
- **Bisect by address.** An environment switch picks which recompiler answers for an address range, so a wrong function can be found by
  bisection instead of guessing.
- **Match the contract first,** then change it once the old recompiler is gone.
- **Measure before and after:** generated size, compile time and peak RAM, and the frame time of the replay routes.

## Risks

- Vector instructions, floating-point exactness and exceptions are where recompilers go wrong; the differential test has to cover them well.
- The current contract has fine points (guard generation, the fast-memory layout) that must be reproduced exactly at first.
- The speed of newly generated code inside the game is unknown until measured.

## What was tried, and what is next

- **`--backend static`** instead of `hybrid`: **tested, no gain.** The output is identical in size (4,543 MB) and shape; both still carry the
  JIT guards.
- **Smaller compilation units** (`unit_insns`): 15000 → 6000 cut the compile step from 99 to 78 min and the peak RAM per compile from 3.5 to
  1.6 GB (now the default); 3000 gained only 3-5% more, so it was not adopted.
- **Where the generated C goes** (measured on one 1.8 MB unit with 1,777 blocks): about 39% is the entry dispatcher of each block, 11% is the
  `_expected[]` array of instruction words, 5% entry labels and 4% guard calls, so **~58% is scaffolding** for entering a block at any
  instruction (needed by a JIT hand-off, which `nxrt` does not use). The translated arithmetic is ~31%, jumps ~8% and memory accesses ~3%.
- **Guards removed (done, now in the installer):** `scripts/slim-export.py` deletes the `_expected[]` arrays and guard calls before compiling.
  Measured on a clean build: generated C 4,543 → 4,132 MB, compile step 78 → 70 min, `nxrt` binary 2.34 → 1.95 GB, and the island replay ran the
  same or slightly faster (37.3-37.5 ms against 37.9-38.2 ms in the transition window). It is smaller than the 18% seen on a single unit.
- **Next:** the entry dispatcher of each block (~39% of the C) is the big remaining piece. Which entries are really needed (return points,
  branch targets, jump tables) must be known exactly before trimming it; the unit test that trimmed it blindly broke that rule on purpose.

## Credit and licensing

We study other recompilers to learn from them. Any code taken from another project is used only under its license and credited. The projects
mentioned in this repository are GPL-3.0, like this one.
