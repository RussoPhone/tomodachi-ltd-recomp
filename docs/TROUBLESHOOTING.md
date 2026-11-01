# Troubleshooting

🇧🇷 [Português](TROUBLESHOOTING.pt-BR.md)

First run `./play.sh --nogui --diagnose`: it checks the GPU, Vulkan, memory, session and missing tools, and says what to fix.
Whatever you try, a [report](TESTING.md#how-to-report-a-problem) (`./install.sh --report`, or **Create bug report** in the launcher) is what lets us help.

## Installing

| Problem | What to do |
|---|---|
| "Missing software" | Run the command from Step 1 of the README again; the message lists what is missing. |
| Out of memory while compiling | You need 16 GB of RAM + 8 GB of swap. Close other programs; the installer resumes where it stopped. |
| "Different version than the supported one" | Your game has an update or is another version. Only 1.0.0 (no updates) works. |
| Step 7 says it needs a desktop session | Run the installer from a terminal inside your desktop, not over SSH. |
| A tool window stays open at step 7 | It should close by itself in seconds (it is the extraction tool, not the game). If not, send the report; this is a known open issue (#13, #16). |
| Not enough disk space | The install uses ~27 GB (35 GB free recommended). `local/` can be deleted to start over. |
| The install is very slow | Plan for 1.5 to 2 hours on a 16-thread machine. Avoid system updates while it compiles. |
| Anything else | `./install.sh --report` and attach the zip to an issue. |

## Playing

| Problem | What to do |
|---|---|
| The launcher does not open | Install Tk (`python3-tk` / `tk`), or start with `./play.sh --nogui`. |
| "No Vulkan device" or a black window | Install your GPU's Vulkan driver (the diagnose command prints the package for your distribution). |
| Very low FPS | On a laptop with two GPUs, make sure the discrete one is used (`DRI_PRIME=1 ./play.sh` on AMD/Intel, `prime-run ./play.sh` on NVIDIA). A software Vulkan device (llvmpipe) is far too slow. |
| Textures look garbled, missing or the wrong colours | Try **Safe mode** (`./play.sh --nogui --safe`). If it fixes it, put that in your report. |
| Crash on start | Send `local/logs/play-nxrt-*.log` (the first lines say which GPU and driver were used). |
| "Your system time has changed since the last time you played" at start | Fixed in Alpha 1.7.1 (the game's clock is now stable between runs). A save last played on an older version may show it once; press OK and it will not come back. |
| Face-paint editor takes ~40 s the first time | Expected: it prepares a cache on your computer. Later openings take seconds. |
| No sound | Check that your sound server works (`pactl info`); send the log if it does. |
| Controls | The keyboard and controllers are supported; remapping is not available yet. |

## Saves

They live in `local/user-nxrt`. The launcher's **Back up now** makes a zip in `local/backups`. Back up before updating.
