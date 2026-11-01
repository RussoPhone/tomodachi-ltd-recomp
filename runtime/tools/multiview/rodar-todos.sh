#!/usr/bin/env bash
# Repete todos os pontos de um conjunto e compara cada um com a sua imagem BASE (a repeticao anterior).
# Como o replay e deterministico, qualquer diferenca depois de mexer no renderer vem da mudanca.
#
# Uso: NXRT_MULTIVIEW=<binario> COMPARADOR=<comparar_regioes.py> tools/multiview/rodar-todos.sh <pasta-do-conjunto> [--atualizar]
#   <pasta-do-conjunto>/pontos.conf: uma linha por ponto:  nome|quadro|pasta-da-captura|limite-%
#   <pasta-do-conjunto>/<nome>.base.ppm: imagem de referencia (criada/atualizada com --atualizar)
#   --atualizar: grava a repeticao atual como nova base (use depois de uma mudanca aprovada).
# Saida: tabela  ponto | % pixels diferentes | tempo | resultado. Codigo de saida 1 se algum ponto passar do limite.
# Veja docs/MULTIVIEW.md. As capturas e imagens contem dados do jogo: nunca vao para o git.
set -u
DIR=${1:?uso: rodar-todos.sh <pasta-do-conjunto> [--atualizar]}; MODO=${2:-}
NXRT_MULTIVIEW=${NXRT_MULTIVIEW:?defina NXRT_MULTIVIEW}; COMPARADOR=${COMPARADOR:?defina COMPARADOR}
[[ -f "$DIR/pontos.conf" ]] || { echo "falta $DIR/pontos.conf" >&2; exit 2; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
FALHAS=0
printf '%-10s %-12s %-9s %s\n' ponto "% diferentes" tempo resultado
while IFS='|' read -r NOME QUADRO CAP LIMITE; do
  [[ -z "${NOME// }" || "$NOME" == \#* ]] && continue
  [[ "$CAP" = /* ]] || CAP="$DIR/$CAP"
  rm -rf "$TMP/d"; mkdir -p "$TMP/d"
  T0=$(date +%s%N)
  NXRT_DUMP_DIR="$TMP/d" NXRT_DUMP_EVERY="$QUADRO" timeout 300 "$NXRT_MULTIVIEW" "$CAP" > "$TMP/$NOME.log" 2>&1
  RC=$?; MS=$(( ($(date +%s%N) - T0) / 1000000 ))
  IMG="$TMP/d/frame-$QUADRO.ppm"
  if (( RC != 0 )) || [[ ! -e "$IMG" ]]; then
    printf '%-10s %-12s %-9s %s\n' "$NOME" - "${MS}ms" "FALHOU (veja $TMP/$NOME.log)"; FALHAS=$((FALHAS+1)); continue
  fi
  BASE="$DIR/$NOME.base.ppm"
  if [[ "$MODO" == --atualizar || ! -e "$BASE" ]]; then
    cp "$IMG" "$BASE"; printf '%-10s %-12s %-9s %s\n' "$NOME" - "${MS}ms" "base gravada"; continue
  fi
  PCT=$(python3 "$COMPARADOR" "$BASE" "$IMG" --tolerancia 8 --regiao tela=0,0,1920,1080 | tail -1 | sed -E 's/.*diferentes ([0-9.]+)%.*/\1/')
  if awk -v p="$PCT" -v l="$LIMITE" 'BEGIN{exit !(p+0 <= l+0)}'; then R=ok; else R="ACIMA DO LIMITE ($LIMITE%)"; FALHAS=$((FALHAS+1)); cp "$IMG" "$DIR/$NOME.ultima.ppm"; fi
  printf '%-10s %-12s %-9s %s\n' "$NOME" "$PCT" "${MS}ms" "$R"
done < "$DIR/pontos.conf"
exit $(( FALHAS > 0 ))
