#!/usr/bin/env bash
# P1: roda o nxrt com o amostrador LD_PRELOAD (tools/perf-preload.c) sob o flock de jogo.
# Uso: tools/perf-run.sh <saida-dir> <save-origem> <input-script> <segundos> [modo: window|offscreen]
# Saída: <dir>/{run.log,samples.bin,threads.csv,maps.txt,save-before.sha256,save-after.sha256}
# O save de origem é copiado para /tmp/p1-save (descartável); o original nunca é tocado.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
out=$1; src=$2; input=$3; secs=$4; mode=${5:-window}
bin=${NXRT_BIN:-$here/build-perf/nxrt}
lib=${PERF_LIB:-$here/build-perf/libperf-preload.so}
game=${NXRT_GAME_DIR:?defina NXRT_GAME_DIR (pasta do jogo instalado)}
save=/tmp/p1-save
mkdir -p "$out"
rm -rf -- "$save" && cp -a -- "$src" "$save" || exit 2
(cd "$save" && find . -type f -print0 | sort -z | xargs -0 sha256sum) >"$out/save-before.sha256"
args=(--game "$game" --user "$save")
[ "$mode" = offscreen ] && args+=(--offscreen)
export NXRT_PERF_OUT="$out" NXRT_INPUT_SCRIPT="$input" NXRT_FRAME_PROFILE=1 NXRT_LOG=${NXRT_LOG:-info}
echo "perf-run: $bin ${args[*]} secs=$secs out=$out" >&2
"$here/tools/jogo-slot.sh" env LD_PRELOAD="$lib" timeout -s INT -k 15 "$secs" "$bin" "${args[@]}" >"$out/run.log" 2>&1
echo "rc=$?" >"$out/exit.txt"
(cd "$save" && find . -type f -print0 | sort -z | xargs -0 sha256sum) >"$out/save-after.sha256"
cmp -s "$out/save-before.sha256" "$out/save-after.sha256" && echo "save: hashes iguais" || echo "save: hashes DIFEREM"
cat "$out/exit.txt"
