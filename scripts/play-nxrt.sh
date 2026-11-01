#!/bin/sh
# Starts the unofficial native port (nxrt). Saves live in local/user-nxrt (your own folder, never overwritten).
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
BIN=local/nxrt/bin/nxrt
GAME=local/install/tomodachi
if [ ! -x "$BIN" ] || [ ! -d "$GAME" ]; then
  echo "The native port is not installed yet. Run ./install.sh first."
  exit 1
fi
mkdir -p local/user-nxrt local/logs
LOG=local/logs/play-nxrt-$(date +%Y%m%d-%H%M%S).log
echo "Starting Tomodachi Life (unofficial native port, alpha). 30 FPS cap."
echo "Note: the first time you open the face-paint editor it takes about 40 seconds; later openings take seconds."
echo "If something goes wrong, the log is in $LOG (attach it to a bug report)."
NXRT_SWAP_INTERVAL_MAX=${NXRT_SWAP_INTERVAL_MAX:-2} NXRT_AUDIO=${NXRT_AUDIO:-1} SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-x11} \
  "$BIN" --game "$GAME" --user local/user-nxrt 2>&1 | tee "$LOG" | grep --line-buffered -E "error|ERROR|warn: |fatal"
exit 0
