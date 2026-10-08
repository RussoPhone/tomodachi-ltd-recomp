#!/bin/sh
# Installs the native build of Tomodachi Life: Living the Dream from YOUR copy of the game.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
exec python3 scripts/install.py "$@"
