#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
bin="$(mktemp /tmp/nxrt-texture-cache-key.XXXXXX)"
trap 'rm -f "$bin"' EXIT
g++ -std=c++20 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/render/texture_cache_key_test.cpp" -o "$bin"
"$bin"
g++ -std=c++20 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/render/image_backing_identity_test.cpp" -o "$bin"
"$bin"
