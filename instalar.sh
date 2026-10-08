#!/bin/sh
# Instala a versão nativa do Tomodachi Life: Living the Dream a partir da SUA cópia do jogo.
cd "$(dirname "$(readlink -f "$0")")" || exit 1
exec python3 scripts/instalar.py "$@"
