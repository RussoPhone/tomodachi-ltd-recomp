# Contributing

Contributions are welcome: open an issue to report a problem or discuss a change, or send a pull request.
Forks are welcome too.

## Ground rules (legal)

- Never commit or attach game files, keys, firmware, dumps, generated C, decompiled code, screenshots of the game,
  frame captures or shader dumps. Everything derived from a copy of the game stays in `local/` (gitignored).
- Do not help anyone obtain games, keys or firmware. Users bring their own dump from their own console.
- Generated or decompiled code is a machine translation. Never present it as the game's original source.
- Code is GPL-3.0-or-later, like suyu.

## How the project works

1. `install.sh` / `install.bat` run `scripts/install.py`: 10 resumable steps (markers in `local/state/`).
2. Pinned upstream: mk8-recomp and its suyu fork (`upstream.lock.json`, checked by `scripts/verify-upstream.py`).
3. All numbered `patches/*.patch` (currently 0001–0009) are applied to that suyu. Each patch explains itself at the top.
4. suyu is built once without the JIT (`scripts/package-standalone.py --backend-only`).
5. The game's code is dumped (`dump-exefs.py`) and exported to C by suyu's AOT exporter over its MCP port
   (`export-aot.py`).
6. The C is compiled with clang under a memory-aware scheduler and object cache (`compile-meter.py`) and linked into
   one executable (`package-standalone.py`).
7. `install-native.py` extracts the game data; `assemble-package.py` builds `local/package/tomodachi/`, intended to
   run using the locally extracted files without the original NSP, keys or firmware. The executable still relies on
   suyu-derived emulation/runtime components; it is not an emulator-free port.

Per-game values live in `adapters/tomodachi/target.json` (expected build IDs, file hashes, export settings).
Windows uses the same Python steps, started by `scripts/windows/install.ps1` (winget tools, MSVC for suyu,
clang-cl for the game's C in upstream's `recomp_modules` project). Windows CI (`.github/workflows/windows.yml`)
attempts steps 1, 3, 4 and 5 without a game. Linux CI (`.github/workflows/linux.yml`) runs syntax checks and
synthetic tests, not the full game build. Neither is an end-to-end gameplay test.

## Tests and review expectations

Run `python3 -m unittest discover -s tests -v` to exercise the Python installer state, object cache (requires a C
compiler), packaging and upstream revision verification using only synthetic files. Run
`python3 -m compileall -q scripts tests` for Python syntax checks. No game dump, keys or firmware are needed.

Changes that affect resumable steps must update the input-fingerprint mapping in `scripts/install.py` and include a
regression test for invalidation. Compiler cache changes should demonstrate both a cache hit for unchanged input and
a miss when a dependency changes. Clearly distinguish synthetic tests, a no-game backend build, and a manually
observed end-to-end install in PR descriptions; do not claim one proves the others.

This project has used **substantial generative-AI assistance** for code, patches, documentation and debugging. AI
output is not proof of correctness and has not been fully independently audited. AI-assisted contributions are
welcome, but contributors must review what they submit, disclose relevant assistance, and attach reproducible test
evidence. Please report limitations and failures as plainly as successes.

## Open work (good first steps)

- **Windows:** nobody has finished steps 6-10 on a real Windows PC yet. Running it and fixing what breaks is very
  valuable.
- **Native graphics:** see `research/README.md`. The shadow NVN→Vulkan renderer draws the game's scenes; the window
  composition bug is the next thing to solve, then a hybrid mode shipped as an opt-in `--native-graphics`.
- **Missing texture formats in the native renderer:** ASTC (decode with suyu's `video_core/textures/astc.h`),
  array/3D/cube textures, mipmaps.
- **A lean runtime:** cut the suyu parts this game never uses (Qt GUI, JIT, unused services), so the game runs on
  a small runtime of its own.
- **Decomp:** `research/tools` has the indexing, naming and Ghidra tooling.

## Patches

Patches are made against pristine suyu plus all earlier patches, and must reproduce the tree exactly:
`research/tools/mkpatch.sh` and `research/tools/verify-patches.sh` do both. Keep one problem per patch, with an
explanation at the top.
