#!/bin/bash
# Lab helper: (re)start the packaged native build with the NVN shadow renderer, one instance only.
#   scripts/nvn-hle-run.sh start [seconds]   stop any running instance, start a new one, wait, print the summary
#   scripts/nvn-hle-run.sh stop
L=$(cd "$(dirname "$0")/.." && pwd); P=$L/local/package/tomodachi; B=$L/upstream/mk8-recomp/build/suyu-static
stop() {
    for pid in $(ps -eo pid=,comm= | awk '$2 ~ /^tomodachi-nativ/ {print $1}'); do kill -TERM "$pid" 2>/dev/null; done
    for _ in $(seq 20); do ps -eo comm= | grep -q '^tomodachi-nativ' || return 0; sleep 0.5; done
    for pid in $(ps -eo pid=,comm= | awk '$2 ~ /^tomodachi-nativ/ {print $1}'); do kill -KILL "$pid" 2>/dev/null; done
}
case "$1" in
stop) stop ;;
start)
    stop
    rm -rf "$L/artifacts/nvn-frames" "$L/artifacts/nvn-spirv"; mkdir -p "$L/artifacts/nvn-spirv"
    ln -f "$B/bin/suyu-cmd-static" "$P/tomodachi-native"
    cd "$P" && SUYU_NVN_HLE=shadow SUYU_NVN_HLE_LOG=$L/artifacts/nvn-hle.log SUYU_NVN_HLE_DUMP_DIR=$L/artifacts/nvn-frames \
        SUYU_NVN_HLE_DUMP_EVERY=${DUMP_EVERY:-300} SUYU_NVN_HLE_SPIRV_DIR=$L/artifacts/nvn-spirv SUYU_NVN_BOOTSTRAP=nnSdk:0x502d10 \
        setsid ./tomodachi-native -g game/main > "$L/artifacts/nvn-hle-run.log" 2>&1 < /dev/null &
    sleep "${2:-60}"
    grep -a "nvn_hle" "$L/artifacts/nvn-hle-run.log" | sed -E 's/\x1b\[[0-9;]*m//g; s/^\[ *([0-9.]+)\][^:]*:[^:]*:[^:]*: nvn_hle: /\1 /; s/-> [^ ]+//' \
        | sort -k2 | uniq -f1 -c | sort -k2 -n | cut -c1-260 | tail -${3:-14}
    ;;
esac
