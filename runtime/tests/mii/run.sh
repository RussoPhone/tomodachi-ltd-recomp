#!/bin/sh
# Builds and runs the Mii default-design regression against build-<slot>/libsuyu_mii.a.
# Usage: tests/mii/run.sh [build-dir]   (default: build-h). Does not touch the game CMake.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
build=${1:-$root/build-h}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
g++ -std=c++20 -I"$root/third_party/suyu" "$root/tests/mii/default_mii_regression.cpp" \
    "$build/libsuyu_mii.a" -o "$out/default_mii_regression"
"$out/default_mii_regression"
