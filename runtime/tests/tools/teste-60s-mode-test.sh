#!/usr/bin/env bash
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$tmp/game" "$tmp/save"
touch "$tmp/nxrt"
chmod +x "$tmp/nxrt"

offscreen=$(NXRT_SLOT=mode-test bash "$repo/tools/teste-60s.sh" \
  -b "$tmp/nxrt" -g "$tmp/game" -s "$tmp/save" -n -m offscreen 2>&1)
grep -F -- "$tmp/nxrt --offscreen --game $tmp/game --user /tmp/mode-test-save" <<<"$offscreen"
if grep -F -- "--headless" <<<"$offscreen"; then
  echo "mode-test: offscreen command also contains --headless" >&2
  exit 1
fi

headless=$(NXRT_SLOT=mode-test bash "$repo/tools/teste-60s.sh" \
  -b "$tmp/nxrt" -g "$tmp/game" -s "$tmp/save" -n 2>&1)
grep -F -- "$tmp/nxrt --headless --game $tmp/game --user /tmp/mode-test-save" <<<"$headless"

if NXRT_SLOT=mode-test bash "$repo/tools/teste-60s.sh" \
  -b "$tmp/nxrt" -g "$tmp/game" -s "$tmp/save" -n -m invalid >/dev/null 2>&1; then
  echo "mode-test: invalid mode was accepted" >&2
  exit 1
fi

if NXRT_SLOT=mode-test bash "$repo/tools/teste-60s.sh" \
  -b "$tmp/nxrt" -g "$tmp/game" -s "$tmp/save" -n -m offscreen -- --headless >/dev/null 2>&1; then
  echo "mode-test: passthrough mode flag was accepted" >&2
  exit 1
fi
