# Changelog

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
