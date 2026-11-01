#!/bin/sh
# Starts the game.
#   ./play.sh            the unofficial native port (nxrt, alpha)   <- default
#   ./play.sh --legacy   the older suyu-based build (kept for comparison; not actively maintained)
cd "$(dirname "$(readlink -f "$0")")" || exit 1
if [ "$1" = "--legacy" ]; then
  shift
  if [ ! -x local/package/tomodachi/run.sh ]; then
    echo "The game is not installed yet. Run ./install.sh first."
    exit 1
  fi
  exec local/package/tomodachi/run.sh "$@"
fi
exec scripts/play-nxrt.sh "$@"
