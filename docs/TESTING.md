# Testing the native port (alpha)

This page says what has been tested, what has not, and how you can help. 🇧🇷 [Português](TESTING.pt-BR.md)

## What this is

An **unofficial native PC port** of *Tomodachi Life: Living the Dream*, built **on your computer** from **your own copy** of the game
(`.nsp`, version 1.0.0) and your own `prod.keys`. The game's ARM code is statically recompiled to x86-64 and runs on **nxrt**, this
project's runtime, with its own NVN → Vulkan renderer. Part of the Switch system services still comes from suyu-derived code.
Nothing from the game, the firmware or your keys is distributed.

## Where it has been tested

| Item | Tested |
|---|---|
| OS | Linux: EndeavourOS (Arch), kernel 7.x |
| CPU / RAM | Intel i5-13450HX, 16 GB RAM + 16 GB swap |
| GPU | NVIDIA GeForce RTX 4050 Laptop (proprietary driver 615.x), Vulkan 1.4 |
| Game | Tomodachi Life: Living the Dream **1.0.0, no updates** |
| By | one person (the maintainer) |

## What works (as observed by the maintainer)

- Boot, title, creating a profile, the island, Mii creation, the market and Residents screens.
- 30 FPS on the island (the cap is 30). Brief drops to about 23 FPS happen when a new scene loads.
- The face-paint editor: the **first** opening takes about 40 s (it prepares a cache in `~/.cache/nxrt`); later openings take a few seconds.
- Saves are kept in `local/user-nxrt` and the original game data is never modified.

## What has NOT been tested (please help)

- **AMD and Intel GPUs**, and NVIDIA GPUs other than the RTX 4050 (the GPU texture decoder and the asynchronous submit path are new).
- **Other distributions** (Fedora, Ubuntu, Debian, Arch derivatives, SteamOS) and Wayland-only sessions.
- **Windows**: not supported yet (the native port is Linux-only for now).
- Long play sessions, other parts of the game nobody has played yet, and saves from a real console.
- A clean installation from the `.nsp` on a fresh machine (the maintainer's own build was done step by step in a lab).

## How to test

1. Check the requirements in the [README](../README.md#what-you-need): Linux, 16 GB RAM + 8 GB swap, **35 GB free**, a Vulkan GPU.
2. Run `./install.sh`, then `./play.sh`.
3. Play for a while. Try: creating a Mii, walking around the island, opening the market, the face-paint editor (twice), and moving between scenes.
4. Optional: `NXRT_ASTC_GPU=0 ./play.sh` turns off the GPU texture decoder, and `NXRT_NO_ASYNC_GRAPHICS=1 ./play.sh` turns off asynchronous submit. If a problem goes away with one of them, say so in your report — it points straight at the cause.
5. To see how fast it runs, look for the `vi: ... fps` lines in `local/logs/play-nxrt-*.log`.

## How to report a problem

Open an [issue](https://github.com/RussoPhone/tomodachi-ltd-recomp/issues/new?template=install-or-play-problem.md) with:

- **Install problem:** run `./install.sh --report` and attach the zip. It contains logs and a system summary (distribution, GPU, driver, tool versions). It does **not** contain game data or keys, and personal folders are replaced by `~`.
- **Play problem:** attach `local/logs/play-nxrt-*.log` of that run, say what you were doing, and add a screenshot if the image looks wrong.
- Mention your GPU and driver (`./play.sh --nogui --diagnose` prints them), and whether `./play.sh --nogui --safe` or turning off the options in step 4 changes anything.
- Also see [COMPATIBILITY.md](COMPATIBILITY.md) and [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

Please do **not** post keys, firmware, the `.nsp` or any copy of the game.

## Known limitations

- 30 FPS cap; dips while scenes load.
- First face-paint opening is slow (~40 s).
- Fullscreen, window size and language options (and the launcher) are new: tried only on the maintainer's machine (Japanese and English text and a custom window size were checked); there is no in-game settings menu yet.
- Alpha: expect crashes and graphical glitches, and do not trust it with the only copy of a save you care about (`local/user-nxrt` is a separate folder, but back it up).
