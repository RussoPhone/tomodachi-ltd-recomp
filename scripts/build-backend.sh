#!/usr/bin/env bash
set -euo pipefail
LAB_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$LAB_ROOT/upstream/mk8-recomp/third_party/suyu"
# SUYU_BUILD_NAME selects the build dir (default suyu); SUYU_NO_JIT=ON builds without Dynarmic.
BUILD="$LAB_ROOT/upstream/mk8-recomp/build/${SUYU_BUILD_NAME:-suyu}"
LOG_SUFFIX="${SUYU_BUILD_NAME:+-$SUYU_BUILD_NAME}"
# Optional extra prefix for dependencies not installed system-wide (e.g. a locally extracted Qt Charts).
EXTRA_PREFIX="${EXTRA_PREFIX:-$( [ -d "$LAB_ROOT/local/deps/usr" ] && echo "$LAB_ROOT/local/deps/usr" || true )}"
mkdir -p "$LAB_ROOT/artifacts"
cmake -S "$SRC" -B "$BUILD" -G Ninja \
 ${EXTRA_PREFIX:+-DCMAKE_PREFIX_PATH="$EXTRA_PREFIX"} -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON -DYUZU_USE_BUNDLED_QT=OFF \
 -DGLSLANGVALIDATOR="$(command -v glslangValidator)" \
 -DYUZU_CMD=ON -DYUZU_TESTS=OFF -DENABLE_WEB_SERVICE=OFF \
 -DYUZU_ROOM=OFF -DYUZU_ROOM_STANDALONE=OFF -DENABLE_QT_TRANSLATION=OFF \
 -DUSE_DISCORD_PRESENCE=OFF -Dfmt_FORCE_BUNDLED=ON -DSUYU_NO_JIT="${SUYU_NO_JIT:-OFF}" \
 2>&1 | tee "$LAB_ROOT/artifacts/configure${LOG_SUFFIX}.log"
cmake --build "$BUILD" --target suyu suyu-cmd --parallel "${BUILD_JOBS:-1}" \
 2>&1 | tee "$LAB_ROOT/artifacts/build${LOG_SUFFIX}.log"
