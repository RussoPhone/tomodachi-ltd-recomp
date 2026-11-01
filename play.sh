#!/bin/sh
# Starts the game.
#   ./play.sh                 opens the launcher (PLAY, settings, save backup, bug report); falls back to starting the game
#   ./play.sh --nogui [opts]  starts the native port directly (--fullscreen, --size WxH, --language CODE)
#   ./play.sh --legacy        the older suyu-based build (kept for comparison; not actively maintained)
cd "$(dirname "$(readlink -f "$0")")" || exit 1
if [ "$1" = "--legacy" ]; then
  shift
  if [ ! -x local/package/tomodachi/run.sh ]; then
    echo "The game is not installed yet. Run ./install.sh first."
    exit 1
  fi
  exec local/package/tomodachi/run.sh "$@"
fi
if [ "$1" = "--nogui" ]; then
  shift
  exec scripts/play-nxrt.sh "$@"
fi
if [ $# -eq 0 ] && { [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; } && python3 -c "import tkinter" 2>/dev/null; then
  exec python3 scripts/launcher.py
fi
exec scripts/play-nxrt.sh "$@"
