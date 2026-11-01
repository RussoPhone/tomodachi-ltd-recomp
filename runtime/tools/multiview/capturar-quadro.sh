#!/usr/bin/env bash
# Roda o jogo até um quadro, grava a captura (opcional) e a imagem apresentada nesse quadro.
# Nada do jogo, do save ou da captura deve ir para o git.
#
# Uso:
#   NXRT=<binario nxrt> GAME=<pasta do jogo> SAVE=<pasta de save descartavel> TARGET=<quadro> \
#     [CAPTURE=<primeiro:quantidade>] [INPUT_SCRIPT=<script de entrada>] [TIMEOUT=600] \
#     tools/multiview/capturar-quadro.sh <pasta-de-saida> [VAR=valor ...]
#
# Saida: <pasta>/dump/frame-<TARGET>.ppm e, com CAPTURE, <pasta>/captura/.
# Exemplo: CAPTURE=1398:3 TARGET=1400 captura os quadros 1398-1400 e para depois do 1400.
# O SAVE e COPIADO para dentro da pasta de saida; o original nunca e alterado.
set -u
OUT=${1:?uso: capturar-quadro.sh <pasta-de-saida> [VAR=valor ...]}; shift
NXRT=${NXRT:?defina NXRT}; GAME=${GAME:?defina GAME}; SAVE=${SAVE:?defina SAVE}; TARGET=${TARGET:?defina TARGET}
TIMEOUT=${TIMEOUT:-600}
[[ -e "$OUT" ]] && { echo "ja existe: $OUT" >&2; exit 2; }
mkdir -p "$OUT/dump" && cp -a "$SAVE" "$OUT/save"
sha256sum "$NXRT" > "$OUT/binary.sha256"
EXTRA=()
[[ -n "${CAPTURE:-}" ]] && EXTRA+=(NXRT_CAPTURE_FRAMES="$CAPTURE" NXRT_CAPTURE_DIR="$OUT/captura")
[[ -n "${INPUT_SCRIPT:-}" ]] && EXTRA+=(NXRT_INPUT_SCRIPT="$INPUT_SCRIPT")
env NXRT_DUMP_DIR="$OUT/dump" NXRT_DUMP_EVERY="$TARGET" "${EXTRA[@]}" "$@" \
  timeout -s INT "$TIMEOUT" "$NXRT" --game "$GAME" --user "$OUT/save" --offscreen > "$OUT/game.log" 2>&1 &
PID=$!
while kill -0 "$PID" 2>/dev/null; do
  if [[ -e "$OUT/dump/frame-$TARGET.ppm" ]]; then sleep 3; kill -INT "$PID" 2>/dev/null || true; break; fi
  sleep 2
done
wait "$PID"
[[ -e "$OUT/dump/frame-$TARGET.ppm" ]] && echo "captura=ok" || { echo "captura=FALTOU (veja $OUT/game.log)" >&2; exit 1; }
