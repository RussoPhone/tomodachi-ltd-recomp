#!/bin/bash
# Lab helper: run the shadow-renderer build under gdb (as its parent, so ptrace is allowed); when the
# frame log stops advancing for STALL seconds, interrupt it and write every thread's backtrace.
L=$(cd "$(dirname "$0")/.." && pwd); P=$L/local/package/tomodachi
"$L/scripts/nvn-hle-run.sh" stop
ln -f "$L/upstream/mk8-recomp/build/suyu-static/bin/suyu-cmd-static" "$P/tomodachi-native"
rm -rf "$L/artifacts/nvn-frames"; mkdir -p "$L/artifacts/nvn-frames"
cd "$P"
SUYU_NVN_HLE=shadow SUYU_NVN_HLE_LOG=$L/artifacts/nvn-hle.log SUYU_NVN_HLE_DUMP_DIR=$L/artifacts/nvn-frames \
SUYU_NVN_HLE_DUMP_EVERY=300 SUYU_NVN_BOOTSTRAP=nnSdk:0x502d10 \
gdb -q -batch -ex "handle SIGSEGV nostop noprint pass" -ex "handle SIGBUS nostop noprint pass" \
    -ex "handle SIGUSR1 nostop noprint pass" -ex "handle SIGUSR2 nostop noprint pass" \
    -ex "handle SIGPIPE nostop noprint pass" -ex "handle SIGINT stop print nopass" -ex run \
    -ex "thread apply all bt 25" --args ./tomodachi-native -g game/main \
    > "$L/artifacts/nvn-hle-gdb.txt" 2>&1 < /dev/null &
GDB=$!
last=0; still=0
while kill -0 $GDB 2>/dev/null; do
    sleep 5
    n=$(grep -a -c "nvn_hle: frame" "$L/artifacts/nvn-hle-gdb.txt")
    if [ "$n" -gt 0 ] && [ "$n" = "$last" ]; then still=$((still + 5)); else still=0; fi
    last=$n
    if [ $still -ge ${STALL:-25} ]; then
        pkill -INT -x tomodachi-nativ
        sleep 60; break
    fi
done
pkill -KILL -x tomodachi-nativ 2>/dev/null; kill $GDB 2>/dev/null
echo "frames logged: $last"
