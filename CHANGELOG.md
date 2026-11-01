# Changelog

## Alpha 1.7.1 — 2026-11-01
- **Fixed:** the game no longer says "Your system time has changed since the last time you played" every time it starts. The game saves the
  system clock context of the last session and compares it at start-up; nxrt rebuilt that context from the run's uptime on every launch, so
  it always looked changed. The context and the steady clock are now stable from one run to the next. A save last played on an older version
  can show the message once.
- **New:** the **Home** key (or the controller's Guide button) closes the game, in place of the Switch's HOME button that some dialogs ask for.

## Alpha 1.7.0 — 2026-10-31
- GPU ASTC texture decoding, faster scene transitions, window size / fullscreen / language options, launcher, install and support tools
  (see the [roadmap](docs/ROADMAP-RECOMPILER.md) and [docs/TESTING.md](docs/TESTING.md)).
