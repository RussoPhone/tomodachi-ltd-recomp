#!/usr/bin/env bash
# Repete uma captura com o nxrt-multiview e compara com a imagem de referencia.
#
# Uso: NXRT_MULTIVIEW=<binario> COMPARADOR=<comparar_regioes.py> TARGET=<quadro> \
#        tools/multiview/repetir.sh <pasta-da-captura> <referencia.ppm> [saida.ppm]
# Imprime o tempo e a % de pixels diferentes (tolerancia 8 por canal, tela inteira).
# Lembre: dois originais do jogo ja diferem 3-6% entre si (ondas, animacao de texto).
set -u
CAP=${1:?uso: repetir.sh <pasta-da-captura> <referencia.ppm> [saida.ppm]}; REF=${2:?falta a referencia}
NXRT_MULTIVIEW=${NXRT_MULTIVIEW:?defina NXRT_MULTIVIEW}; COMPARADOR=${COMPARADOR:?defina COMPARADOR}; TARGET=${TARGET:?defina TARGET}
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
T0=$(date +%s%N)
NXRT_DUMP_DIR="$TMP" NXRT_DUMP_EVERY="$TARGET" timeout 300 "$NXRT_MULTIVIEW" "$CAP" > "$TMP/replay.log" 2>&1 \
  || { echo "replay falhou:"; tail -3 "$TMP/replay.log"; exit 1; }
MS=$(( ($(date +%s%N) - T0) / 1000000 ))
IMG="$TMP/frame-$TARGET.ppm"; [[ -e "$IMG" ]] || { echo "sem imagem no quadro $TARGET"; exit 1; }
[[ -n "${3:-}" ]] && cp "$IMG" "$3"
echo "tempo=${MS}ms"
python3 "$COMPARADOR" "$REF" "$IMG" --tolerancia 8 --regiao tela=0,0,1920,1080 | tail -1
