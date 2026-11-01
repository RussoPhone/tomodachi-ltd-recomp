#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
bin="$(mktemp /tmp/nxrt-fs-write-contract.XXXXXX)"
trap 'rm -f "$bin"' EXIT
g++ -std=c++20 -Wall -Wextra -Werror -I"$root/src" \
    "$root/tests/services/fs_write_contract_test.cpp" -o "$bin"
"$bin"
