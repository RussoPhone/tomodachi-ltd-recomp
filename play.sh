#!/bin/sh
# Starts the game. Options: --fullscreen   --scale 2 (sharper image; 1, 1.5, 2, 3 or 4)
cd "$(dirname "$(readlink -f "$0")")" || exit 1
if [ ! -x local/package/tomodachi/run.sh ]; then
  echo "The game is not installed yet. Run ./install.sh first."
  exit 1
fi
exec local/package/tomodachi/run.sh "$@"
