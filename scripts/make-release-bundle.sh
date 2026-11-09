#!/usr/bin/env bash
# Builds the files attached to a GitHub Release: source bundle (.tar.gz and .zip) + SHA256SUMS.
# Contains only tracked project files: no game, keys, firmware or build output.
# Usage: scripts/make-release-bundle.sh <tag-or-ref> [output-dir]
set -euo pipefail
ref="${1:?usage: $0 <tag-or-ref> [output-dir]}"
out="${2:-release-assets}"
name="tomodachi-ltd-recomp-${ref#v}"
cd "$(git rev-parse --show-toplevel)"
git rev-parse --verify -q "$ref^{commit}" >/dev/null || { echo "unknown ref: $ref" >&2; exit 1; }
mkdir -p "$out"
git archive --format=tar.gz --prefix="$name/" -o "$out/$name.tar.gz" "$ref"
git archive --format=zip --prefix="$name/" -o "$out/$name.zip" "$ref"
if tar -tzf "$out/$name.tar.gz" | grep -E '(^|/)(prod\.keys|title\.keys|.*\.nsp|.*\.xci|local/)' >/dev/null; then
  echo "refusing: bundle contains game/key-like files" >&2; rm -f "$out/$name".*; exit 1
fi
(cd "$out" && sha256sum "$name.tar.gz" "$name.zip" > SHA256SUMS)
echo "ok: $out/$name.tar.gz $out/$name.zip $out/SHA256SUMS"
