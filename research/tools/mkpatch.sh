#!/usr/bin/env bash
# Usage: mkpatch.sh <out.patch> <base patch count N> <files...>
# Create a diff of the working suyu tree against pristine suyu + patches[0001..N].
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
S=$ROOT/upstream/mk8-recomp/third_party/suyu

if (($# < 3)); then
    echo "usage: $0 <out.patch> <base patch count N> <files...>" >&2
    exit 2
fi

out=$1
upto=$2
if [[ $out != /* ]]; then
    out=$PWD/$out
fi
if [[ ! $upto =~ ^[0-9]+$ ]]; then
    echo "patch count must be a non-negative integer: $upto" >&2
    exit 2
fi
shift 2

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
if ((upto > ${#patches[@]})); then
    echo "requested $upto base patches, but only ${#patches[@]} exist" >&2
    exit 1
fi

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T"
git init -q

for f in "$@"; do
    mkdir -p "$(dirname "$f")"
    if git -C "$S" show "HEAD:$f" >"$f" 2>/dev/null; then
        :
    else
        rm -f "$f"
    fi
done

for ((i = 0; i < upto; i++)); do
    p=${patches[i]}
    include_args=()
    for f in "$@"; do
        include_args+=("--include=$f")
    done
    if ! git apply "${include_args[@]}" "$p"; then
        echo "could not apply base patch $(basename "$p")" >&2
        exit 1
    fi
done

mkdir -p A B
for f in "$@"; do
    if [[ -f $f ]]; then
        mkdir -p "A/$(dirname "$f")"
        cp "$f" "A/$f"
    fi
    if [[ -f $S/$f ]]; then
        mkdir -p "B/$(dirname "$f")"
        cp "$S/$f" "B/$f"
    fi
done

set +e
git diff --no-index A B | sed -E 's#^(--- |\+\+\+ )(a|b)/[AB]/#\1\2/#; s#^diff --git a/[AB]/(.*) b/[AB]/#diff --git a/\1 b/#' >"$out"
diff_status=$?
set -e
if ((diff_status > 1)); then
    echo "could not write patch to $out" >&2
    exit "$diff_status"
fi
