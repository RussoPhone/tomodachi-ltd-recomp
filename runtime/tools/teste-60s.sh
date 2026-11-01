#!/usr/bin/env bash
# Roda o nxrt por até 60 s sob o flock global do jogo e triagem o log.
#
# Uso: tools/teste-60s.sh [-b BIN] [-g PASTA_JOGO] [-s SAVE_ORIGEM] [-t SEG] [-l LOG] [-m headless|offscreen] [-- args extras do nxrt]
#   -b  binário (padrão: build-<slot>/nxrt do worktree; slot = NXRT_SLOT ou "k")
#   -g  pasta do jogo (padrão: $NXRT_GAME_DIR, senão $LAB/local/install/tomodachi)
#   -s  save de origem; é COPIADO para /tmp/<slot>-save, o original nunca é tocado
#       (padrão: $NXRT_SAVE_SRC; sem origem, reaproveita /tmp/<slot>-save se existir)
#   -t  timeout em segundos (padrão 60)
#   -l  arquivo de log (padrão /tmp/<slot>-teste-60s.log)
#   -m  modo do runtime: headless (padrão) ou offscreen
#   -n  só mostra o comando, não executa
# Variáveis NXRT_* do ambiente (NXRT_LOG etc.) passam para o nxrt.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
slot=${NXRT_SLOT:-k}
bin=$here/build-$slot/nxrt
game=${NXRT_GAME_DIR:-${LAB:+$LAB/local/install/tomodachi}}
save_src=${NXRT_SAVE_SRC:-}
secs=60
log=/tmp/$slot-teste-60s.log
dry=0
mode=headless
while getopts "b:g:s:t:l:m:nh" o; do
  case $o in
    b) bin=$OPTARG ;; g) game=$OPTARG ;; s) save_src=$OPTARG ;;
    t) secs=$OPTARG ;; l) log=$OPTARG ;; m) mode=$OPTARG ;; n) dry=1 ;;
    h|*) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
[ "${1:-}" = "--" ] && shift

save=/tmp/$slot-save
lock=/tmp/nxrt-game.lock
die() { echo "teste-60s: $*" >&2; exit 2; }

case $mode in
  headless|offscreen) mode_arg=--$mode ;;
  *) die "modo inválido: $mode (use headless ou offscreen)" ;;
esac
for arg in "$@"; do
  case $arg in
    --headless|--offscreen) die "selecione o modo com -m; não passe $arg após --" ;;
  esac
done

[ -x "$bin" ] || die "binário não executável: $bin"
[ -n "$game" ] && [ -d "$game" ] || die "pasta do jogo ausente (use -g ou NXRT_GAME_DIR): ${game:-<vazio>}"
if [ -n "$save_src" ]; then
  [ -d "$save_src" ] || die "save de origem inexistente: $save_src"
  [ "$(realpath "$save_src")" != "$(realpath -m "$save")" ] || die "origem e cópia são o mesmo diretório"
elif [ ! -d "$save" ]; then
  die "sem save: passe -s ORIGEM (ou NXRT_SAVE_SRC) para criar $save"
fi

cmd=("$bin" "$mode_arg" --game "$game" --user "$save" "$@")
echo "teste-60s: ${cmd[*]}" >&2
echo "teste-60s: log=$log timeout=${secs}s lock=$lock" >&2
[ $dry = 1 ] && exit 0

if [ -n "$save_src" ]; then
  rm -rf -- "$save" && cp -a -- "$save_src" "$save" || die "falha ao copiar o save"
fi

# flock -w: espera até 120 s pelo jogo de outro agente; o timeout só conta
# depois de obtido o lock. -s INT dá chance ao nxrt de encerrar limpo.
flock -w 120 "$lock" timeout -s INT -k 10 "$secs" "${cmd[@]}" >"$log" 2>&1
rc=$?
case $rc in
  1) [ ! -s "$log" ] && echo "teste-60s: flock não obteve o lock em 120 s" >&2 ;;
  124) echo "teste-60s: encerrado por timeout (${secs}s), esperado" >&2 ;;
esac
echo "teste-60s: rc=$rc, $(wc -l <"$log") linhas em $log" >&2

python3 "$here/tools/triagem-log.py" "$log"
exit $rc
