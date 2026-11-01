#!/bin/sh
# Starts the unofficial native port (nxrt). Saves live in local/user-nxrt (your own folder, never overwritten).
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1
for arg in "$@"; do
  if [ "$arg" = "--diagnose" ]; then exec python3 scripts/doctor.py; fi
done
BIN=local/nxrt/bin/nxrt
GAME=local/install/tomodachi
if [ ! -x "$BIN" ] || [ ! -d "$GAME" ]; then
  echo "The native port is not installed yet. Run ./install.sh first."
  exit 1
fi
# Options: --fullscreen   --size 1920x1080   --language en-GB   --safe   --diagnose   (F11 toggles fullscreen in the game)
#   --safe      conservative graphics path for GPUs/drivers nobody has tested (slower, but fewer things can go wrong)
#   --diagnose  checks this computer (GPU, Vulkan, memory, session) and exits
while [ $# -gt 0 ]; do
  case "$1" in
    --fullscreen) export NXRT_FULLSCREEN=1 ;;
    --size) shift; export NXRT_WINDOW_SIZE="$1" ;;
    --language) shift; export NXRT_LANGUAGE="$1" ;;
    --safe) export NXRT_SAFE_MODE=1 ;;
    --diagnose) exec python3 scripts/doctor.py ;;
    *) echo "Unknown option: $1 (use --fullscreen, --size WxH, --language CODE, --safe or --diagnose)"; exit 2 ;;
  esac
  shift
done
mkdir -p local/user-nxrt local/logs
LOG=local/logs/play-nxrt-$(date +%Y%m%d-%H%M%S).log
echo "Starting Tomodachi Life (unofficial native port, alpha). 30 FPS cap."
echo "Note: the first time you open the face-paint editor it takes about 40 seconds; later openings take seconds."
echo "If something goes wrong, the log is in $LOG (attach it to a bug report)."
NXRT_SWAP_INTERVAL_MAX=${NXRT_SWAP_INTERVAL_MAX:-2} NXRT_AUDIO=${NXRT_AUDIO:-1} SDL_VIDEODRIVER=${SDL_VIDEODRIVER:-x11} \
  "$BIN" --game "$GAME" --user local/user-nxrt 2>&1 | tee "$LOG" | grep --line-buffered -E "error|ERROR|warn: |fatal"
exit 0
