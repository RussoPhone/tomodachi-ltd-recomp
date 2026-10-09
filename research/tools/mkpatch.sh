#!/bin/bash
# usage: mkpatch.sh <out.patch> <base patches glob count N> <files...>  -- diff of current tree vs pristine+patches[0001..N]
L=$(cd "$(dirname "$0")/.." && pwd); S=$L/upstream/mk8-recomp/third_party/suyu
out=$1; upto=$2; shift 2
T=$(mktemp -d); cd $T && git init -q
for f in "$@"; do mkdir -p $(dirname $f); git -C $S show HEAD:$f > $f 2>/dev/null || rm -f $f; done
for p in $(ls $L/patches/*.patch | sort | head -n $upto); do incl=(); for f in "$@"; do incl+=(--include=$f); done; git apply "${incl[@]}" $p 2>/dev/null; done
mkdir -p A B; for f in "$@"; do if [ -f $f ]; then mkdir -p A/$(dirname $f); cp $f A/$f; fi; mkdir -p B/$(dirname $f); cp $S/$f B/$f; done
git diff --no-index A B | sed -E 's#^(--- |\+\+\+ )(a|b)/[AB]/#\1\2/#; s#^diff --git a/[AB]/(.*) b/[AB]/#diff --git a/\1 b/#' > $out
rm -rf $T
