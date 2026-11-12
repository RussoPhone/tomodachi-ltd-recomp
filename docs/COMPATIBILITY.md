# Compatibility

🇧🇷 [Português](COMPATIBILITY.pt-BR.md)

What has been tried on which hardware. **Only the first row has been tested by the maintainer**; every other row is a gap that you can fill.
Add yours with a [compatibility report](https://github.com/RussoPhone/tomodachi-ltd-recomp/issues/new?template=compatibility-report.md).

## Tested

| Status | GPU / driver | OS | Result | Reported by |
|---|---|---|---|---|
| ✅ Works | NVIDIA GeForce RTX 4050 Laptop, proprietary 615.x, Vulkan 1.4 | EndeavourOS (Arch), kernel 7.x, Wayland session via XWayland | Full install; 30 FPS on the island; brief dips to ~23 FPS while scenes load | maintainer |

## Windows: first step done

The `nxrt` runtime now **compiles and links on Windows** (GitHub Actions, `windows-2022`, clang, against stand-ins for the game side; see
`.github/workflows/nxrt-windows.yml`). That is all it proves: nothing has been run on Windows, the game-side build (the recompiler and the
installer) is still Linux-only, and there is no Windows installer. Windows is **not supported yet**.

## Not tested yet (help wanted)

| What | Why it matters |
|---|---|
| AMD GPUs (Mesa RADV) | Most requested; the GPU texture decoder and asynchronous submit are new code |
| Intel GPUs (Mesa ANV) | Integrated GPUs may be too slow for 30 FPS; we do not know |
| Other NVIDIA GPUs (GTX 10/16 series, RTX 20/30/40) | Different generations, same driver family |
| Ubuntu, Debian, Fedora, openSUSE, CachyOS, Manjaro | The install commands for these are written but not verified |
| SteamOS / Steam Deck | Untested; storage and the compile time may be a problem. The system is read-only: build and play inside a distrobox (see below) |
| X11-only sessions, KDE, GNOME, other desktops | Window and fullscreen handling |
| Hybrid laptops (NVIDIA + Intel/AMD) | The port picks the discrete GPU first; check `GPU:` in `./play.sh --nogui --diagnose` |

## How to check your own machine

```bash
./play.sh --nogui --diagnose      # GPU, Vulkan version, memory, session, missing tools
./play.sh --nogui --safe          # conservative graphics path, if the picture looks wrong
```

In the launcher: **Check my computer** and the **Safe mode** checkbox do the same.

## What the port needs

- Linux with a Vulkan 1.2 (or newer) GPU driver. A software-only Vulkan device (llvmpipe) is far too slow.
- 16 GB of RAM and 8 GB of swap to install (compiling is memory-hungry), ~27 GB of disk after install (35 GB free recommended).
- Your own copy of the game (`.nsp`, version 1.0.0) and `prod.keys`.

## What safe mode changes

`NXRT_SAFE_MODE=1` (or `--safe`) turns off the newer, faster paths: asynchronous graphics submit, asynchronous compute flush and the
GPU texture decoder (textures are decoded on the CPU instead). It is slower and is meant to answer one question: *does the problem go away?*
If it does, say so in your report: it points straight at the cause.

## SteamOS / Steam Deck

SteamOS keeps its system read-only, so the build tools cannot be installed on it. Use a distrobox container:

```bash
distrobox create --name tomodachi --image archlinux:latest
distrobox enter tomodachi
```

Inside it, follow "Step 1" of the README, then run `./install.sh` and `./play.sh` from the same container. The Deck has 16 GB of RAM at most and little free space: the install wants about 35 GB free and a lot of swap, and the compile can take hours. If it does not fit, build on another PC and copy the `local/` folder.
