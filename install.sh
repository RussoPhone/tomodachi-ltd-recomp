#!/bin/sh
# Builds the experimental PC recompilation (with suyu-derived runtime) from YOUR copy of the game.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
exec python3 scripts/install.py "$@"
