#!/bin/sh
# Starts the game.
#   ./play.sh                 opens the launcher (PLAY, settings, save backup, bug report); falls back to starting the game
#   ./play.sh --nogui [opts]  starts the native port directly (--fullscreen, --size WxH, --language CODE)
#   ./play.sh --shortcut      adds "Tomodachi-LTD-Recomp" to your applications menu (opens the launcher); --remove-shortcut undoes it
cd "$(dirname "$(readlink -f "$0")")" || exit 1
case "$1" in
  --shortcut) exec python3 scripts/launcher.py --install-shortcut ;;
  --remove-shortcut) exec python3 scripts/launcher.py --remove-shortcut ;;
esac
if [ "$1" = "--nogui" ]; then
  shift
  exec scripts/play-nxrt.sh "$@"
fi
if [ $# -eq 0 ] && { [ -n "$DISPLAY" ] || [ -n "$WAYLAND_DISPLAY" ]; } && python3 -c "import tkinter" 2>/dev/null; then
  exec python3 scripts/launcher.py
fi
exec scripts/play-nxrt.sh "$@"
