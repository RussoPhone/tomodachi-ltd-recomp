#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tmp="$(mktemp -d /tmp/nxrt-install-preflight.XXXXXX)"
trap 'rm -rf -- "$tmp"' EXIT

cat >"$tmp/pkg-config" <<'EOF'
#!/usr/bin/env bash
[[ "$1" == --exists ]] || exit 2
case "$2" in
    fmt|sdl2|vulkan) exit 0 ;;
    *) exit 1 ;;
esac
EOF
chmod +x "$tmp/pkg-config"

set +e
output="$(NXRT_PKG_CONFIG="$tmp/pkg-config" \
    NXRT_PKG_MANAGER=unknown \
    NXRT_QT_CMAKE_DIRS="$tmp/no-qt" \
    "$root/scripts/install.sh" 2>&1)"
status=$?
set -e

if ((status == 0)); then
    echo "FAIL: installer unexpectedly proceeded without user inputs" >&2
    exit 1
fi
if [[ "$output" == *"libavcodec"* || "$output" == *"libavfilter"* || \
      "$output" == *"libavutil"* || "$output" == *"Qt6GuiPrivate"* ]]; then
    echo "FAIL: installer requires unrelated FFmpeg/Qt development packages" >&2
    printf '%s\n' "$output" >&2
    exit 1
fi
if [[ "$output" != *"informe --game-export"* ]]; then
    echo "FAIL: installer stopped before validating required user inputs" >&2
    printf '%s\n' "$output" >&2
    exit 1
fi

echo "ALL OK"
