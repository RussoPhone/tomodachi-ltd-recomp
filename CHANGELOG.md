# Changelog

## Unreleased
- Installer: fixed the game export (step 7) stopping at about 970 of 1650 files on Linux (issue #17). The exporter keeps every generated file open at once
  and the default limit is 1024 open files per process; the installer now raises that limit before starting the export, and stops with the
  command to run if the system does not allow enough.
- **Launcher, new look.** The window is redrawn in the project's style (logo, pill buttons, cloud dialogs, the game's quick bounce on press), still Tk only.
  The Controls button is now a sheet with Keyboard, Mouse and Controller tabs. Lists (window size, language) close with a click outside or Esc and
  work with the arrow keys and the wheel. The window redraws while it is resized.
- **Launcher shows the installed build's version**, read from `local/nxrt/bin/nxrt` itself (not from the source folder, which can be newer), and
  tells when a newer release exists on GitHub, with how to update. It never updates anything by itself, asks GitHub at most every 6 hours in the
  background (read-only, cached, silent when offline), and *Check for new builds* switches it off.
- **Launcher saves.** Several named saves: **Main** is the save you already have (`local/user-nxrt`, never moved), *+ New save…* starts a fresh game in
  `local/profiles/<name>`. The launcher opens on the save you played last. Backups have readable names and times ("Save point · 12 min ago"), the three
  newest are on the main window, and *All saves…* restores (a "Before restore" copy is made first), renames and deletes. While you play it takes a save
  point each time the game writes a save and goes quiet (at most one every 3 minutes), and when the game closes it says how long before closing your last save
  was, offering to go back if the game was killed right after writing. It cannot recover progress that was never saved inside the game.
- **Launcher languages:** only the 13 the game has text for. Portuguese and Russian were removed (the game showed English for them); a language saved by an
  older launcher that is not in the list goes back to English (US). The README no longer lists `pt-BR` and `ru` for `--language`.
- **Menu entry:** search "Tomodachi LTD Recomp". `./play.sh --shortcut` / `--remove-shortcut`; the installer uses the same code.
- `play-nxrt.sh`: new `--user DIR` (save folder) and the game's exit code is written to `$NXRT_EXIT_FILE` (the pipe used to swallow it, so a killed game
  looked like a normal exit).
- Launcher fixes: a dialog's confirm button no longer stays on screen after closing, a button under the pointer no longer makes the window spin at 100% CPU,
  and if the window cannot open it says so and points to `./play.sh --nogui`.
- Installer: the game export (step 7) is now closed only after its files stopped changing, and it checks that every source listed by a module
  exists. Before, suyu was stopped as soon as it said "done" while the biggest module was still being written, which left `main` without
  `recomp_runtime.h/.c` and some `src/recompiled_main_*.c` on a fast machine (issue #17). Step 9 also stops early, with the fix, if a source is missing.
- Installer: `./install.sh --redo 7` runs step 7 and every later step again (needed after deleting `local/aot`).
- Installer: when the game export leaves a module without `recomp_runtime.h/.c` (seen once, on `main` only), the files are copied from another
  module, since they are identical in all of them. If no module has them, the install stops at step 9 with a clear message.

## Alpha 1.7.4 — 2026-11-03
- **Fixed: the game's clock showed the wrong time.** Since 1.7.1 the game read "2026-10-28 plus the time you had played" instead of the real date and hour (it
  derives its time from the system counter, and only the clock offset had been fixed). The counter now starts at the difference to the host
  clock, as on a console that has been on for days, so the date and hour match the computer and the "system time has changed" message stays away.
  A save last played on 1.7.1 to 1.7.3 holds the wrong date, so the first start can see several days pass at once.
- **Fixed: running out of video memory when other programs use the GPU.** Texture eviction now follows what the driver says is left for this
  process (VK_EXT_memory_budget), not half of the card, so a browser or compositor holding 2 GB of a 6 GB card no longer makes allocations fail.
- Diagnostics: `NXRT_NO_CNTPCT_BASE=1` (counter from zero; the game's clock is then wrong), `NXRT_NVN_TIME=1` (time spent in the NVN layer per call), `NXRT_NO_PACE=1` (no vsync sleep).

## Alpha 1.7.3 — 2026-11-02
- **Fixed:** long sessions no longer exhaust video memory. Textures that were not used recently are released (least recently used first) once
  images hold too much device memory, which avoids the driver crash after many scenes. Tested in a session of more than 20 minutes.
- The launcher now backs the save up automatically before every play session (newest 10 kept in `local/backups/auto-*.zip`).

## Alpha 1.7.2 — 2026-11-01
- **Experimental mouse support** (awkward in some menus; it will be improved over time): edge scrolling and middle-button drag to move the island
  camera, the wheel zooms, right click is B, **F8** captures the mouse as an analog stick (hand follows it where the game shows its hand;
  F6/F7 sensitivity, acceleration). See [docs/CONTROLS.md](docs/CONTROLS.md). The Mii creator does not respond well to the mouse yet.
- **Fixed:** a crash right after finishing a Mii: image creation ignored a failed device-memory allocation, which crashed the NVIDIA driver. Every
  Vulkan step is checked now, a failure skips the texture instead of crashing, and the log says how much memory images hold.
- The installer inserts a call at the game's hand-pointer update (`scripts/slim-export.py`) so the mouse can steer the hand.

## Alpha 1.7.1 — 2026-11-01
- **Fixed:** the game no longer says "Your system time has changed since the last time you played" every time it starts. The game saves the
  system clock context of the last session and compares it at start-up; nxrt rebuilt that context from the run's uptime on every launch, so
  it always looked changed. The context and the steady clock are now stable from one run to the next. A save last played on an older version
  can show the message once.
- **New:** the **Home** key (or the controller's Guide button) closes the game, in place of the Switch's HOME button that some dialogs ask for.

## Alpha 1.7.0 — 2026-10-31
- GPU ASTC texture decoding, faster scene transitions, window size / fullscreen / language options, launcher, install and support tools
  (see the [roadmap](docs/ROADMAP-RECOMPILER.md) and [docs/TESTING.md](docs/TESTING.md)).
