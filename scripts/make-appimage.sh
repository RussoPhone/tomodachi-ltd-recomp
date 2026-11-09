#!/usr/bin/env bash
# Builds the AppImage of a tag/ref: the project's own files plus a small AppRun (see packaging/appimage/AppRun).
# Contains no game, keys or firmware. Needs appimagetool (APPIMAGETOOL=/path, or on PATH).
# Usage: scripts/make-appimage.sh <tag-or-ref> [output-dir]
set -euo pipefail
ref="${1:?usage: $0 <tag-or-ref> [output-dir]}"
out="$(realpath -m "${2:-release-assets}")"
tool="${APPIMAGETOOL:-$(command -v appimagetool || true)}"
[ -x "$tool" ] || { echo "appimagetool not found (set APPIMAGETOOL)" >&2; exit 1; }
cd "$(git rev-parse --show-toplevel)"
git rev-parse --verify -q "$ref^{commit}" >/dev/null || { echo "unknown ref: $ref" >&2; exit 1; }
version="${ref#v}"
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT
app="$work/AppDir"; share="$app/usr/share/tomodachi-ltd-recomp"
mkdir -p "$share/project" "$out"
git archive "$ref" | tar -x -C "$share/project"
if find "$share" \( -name 'prod.keys' -o -name 'title.keys' -o -name '*.nsp' -o -name '*.xci' -o -name local \) | grep -q .; then
  echo "refusing: game/key-like files in the AppDir" >&2; exit 1
fi
printf '%s' "$version" > "$share/APPIMAGE_VERSION"
git show "$ref:packaging/appimage/AppRun" > "$app/AppRun"; chmod 755 "$app/AppRun"
git show "$ref:packaging/appimage/tomodachi-ltd-recomp.desktop" > "$app/tomodachi-ltd-recomp.desktop"
git show "$ref:assets/icon.svg" > "$app/tomodachi-ltd-recomp.svg"; cp "$app/tomodachi-ltd-recomp.svg" "$app/.DirIcon"
file="$out/Tomodachi-LTD-Recomp-$version-x86_64.AppImage"
ARCH=x86_64 "$tool" --appimage-extract-and-run --no-appstream "$app" "$file" >&2
chmod 755 "$file"
(cd "$out" && sha256sum "$(basename "$file")" > "$(basename "$file").sha256")
echo "ok: $file"
