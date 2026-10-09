#!/bin/bash
# Check that pristine suyu + every lab patch reproduces the working tree, for the files the patches touch.
L=$(cd "$(dirname "$0")/.." && pwd); S=$L/upstream/mk8-recomp/third_party/suyu
files=$(grep -h '^+++ b/' $L/patches/*.patch | sed 's#^+++ b/##; s#\t.*##; s#[[:space:]].*##' | sort -u)
T=$(mktemp -d); cd $T && git init -q
for f in $files; do mkdir -p "$(dirname $f)"; git -C $S show HEAD:$f > $f 2>/dev/null || rm -f $f; done
for p in $(ls $L/patches/*.patch | sort); do git apply $p || { echo "FAIL $(basename $p)"; exit 1; }; done
bad=0; for f in $files; do cmp -s $f $S/$f || { echo "DIFF $f"; bad=1; }; done
rm -rf $T; [ $bad = 0 ] && echo "all $(ls $L/patches/*.patch | wc -l) patches reproduce the tree ($(echo $files | wc -w) files)"; exit $bad
