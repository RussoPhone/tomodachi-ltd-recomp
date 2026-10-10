#!/usr/bin/env bash
# Check that pristine suyu + every lab patch reproduces the working tree for touched files.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
S=$ROOT/upstream/mk8-recomp/third_party/suyu

if [[ ! -d $S/.git && ! -f $S/.git ]]; then
    echo "upstream suyu checkout not found: $S" >&2
    exit 1
fi

shopt -s nullglob
patches=("$ROOT"/patches/*.patch)
shopt -u nullglob
if ((${#patches[@]} == 0)); then
    echo "no patches found in $ROOT/patches" >&2
    exit 1
fi

files=()
declare -A seen=()
for p in "${patches[@]}"; do
    while IFS=$'\t' read -r _added _deleted path; do
        [[ -n $path ]] || continue
        if [[ -z ${seen[$path]+present} ]]; then
            seen[$path]=1
            files+=("$path")
        fi
    done < <(git apply --numstat "$p")
done
if ((${#files[@]} == 0)); then
    echo "no changed files found in $ROOT/patches" >&2
    exit 1
fi

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T"
git init -q
for f in "${files[@]}"; do
    mkdir -p "$(dirname "$f")"
    if git -C "$S" show "HEAD:$f" >"$f" 2>/dev/null; then
        :
    else
        rm -f "$f"
    fi
done

for p in "${patches[@]}"; do
    if ! git apply "$p"; then
        echo "FAIL $(basename "$p")" >&2
        exit 1
    fi
done

bad=0
for f in "${files[@]}"; do
    patched=$T/$f
    working=$S/$f
    if [[ -e $patched || -L $patched ]]; then
        if [[ ! -e $working && ! -L $working ]] || ! cmp -s "$patched" "$working"; then
            echo "DIFF $f"
            bad=1
        fi
    elif [[ -e $working || -L $working ]]; then
        echo "DIFF $f"
        bad=1
    fi
done
if ((bad != 0)); then
    exit 1
fi
printf 'all %d patches reproduce the tree (%d files)\n' "${#patches[@]}" "${#files[@]}"
