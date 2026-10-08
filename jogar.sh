#!/bin/sh
# Abre o jogo. Opções: --fullscreen (tela cheia)  --scale 2 (mais nitidez; 1, 1.5, 2, 3 ou 4)
cd "$(dirname "$(readlink -f "$0")")" || exit 1
if [ ! -x local/package/tomodachi/run.sh ]; then
  echo "O jogo ainda não foi instalado. Rode primeiro: ./instalar.sh"
  exit 1
fi
exec local/package/tomodachi/run.sh "$@"
