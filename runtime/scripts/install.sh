#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Compila o nxrt na máquina do usuário, a partir do export do recompilador dele.
# O binário resultante contém código do jogo: não distribuir.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREFIX="${HOME:?HOME não definido}/.local/share/nxrt"
BUILD_DIR="${ROOT}/build-install"
GAME_EXPORT="${NXRT_GAME_EXPORT:-}"
RECOMP_BUILD="${NXRT_RECOMP_BUILD:-}"
BOOST_ROOT="${NXRT_BOOST_ROOT:-}"
CPU_COUNT="$(nproc 2>/dev/null || echo 2)"
if [[ "$CPU_COUNT" =~ ^[1-9][0-9]*$ ]] && ((CPU_COUNT < 2)); then
    JOBS="$CPU_COUNT"
else
    JOBS=2
fi

die() { echo "erro: $*" >&2; exit 1; }

usage() {
    cat <<'EOF'
uso: scripts/install.sh --game-export DIR --recomp-build DIR --boost-root DIR [opções]

  --game-export DIR   exefs/ do export do recompilador (contém recomp_registration.c)
  --recomp-build DIR  diretório 'recomp' do build estático (contém rtld/, main/, sdk/)
  --boost-root DIR    árvore de fonte do Boost (contém libs/*/include)
  --prefix DIR        destino da instalação (padrão: ~/.local/share/nxrt)
  --build-dir DIR     diretório de build (padrão: build-install/ na raiz do repo)
  -j N                paralelismo do build (padrão: até 2, para reduzir uso de RAM)
  -h, --help          mostra esta ajuda

Também aceitos por variável de ambiente: NXRT_GAME_EXPORT, NXRT_RECOMP_BUILD, NXRT_BOOST_ROOT.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game-export|--recomp-build|--boost-root|--prefix|--build-dir|-j)
            option="$1"
            [[ $# -ge 2 ]] || die "opção $option exige um valor"
            [[ -n "$2" ]] || die "opção $option exige um valor"
            case "$option" in
                --game-export) GAME_EXPORT="$2" ;;
                --recomp-build) RECOMP_BUILD="$2" ;;
                --boost-root) BOOST_ROOT="$2" ;;
                --prefix) PREFIX="$2" ;;
                --build-dir) BUILD_DIR="$2" ;;
                -j) JOBS="$2" ;;
            esac
            shift 2
            ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

# v1: só Linux. Windows entra depois (pendência 5 do HANDOFF).
[[ "$(uname -s)" == "Linux" ]] || die "v1 só suporta Linux; Windows é a próxima etapa"

# CMake is the source of truth for development dependencies. A pkg-config/Qt
# precheck here can reject valid builds that use CMake package configs, and the
# runtime does not depend on the FFmpeg or Qt modules used by the old check.

for tool in cmake ninja clang clang++; do
    command -v "$tool" >/dev/null 2>&1 || die "ferramenta ausente no PATH: $tool"
done

[[ -n "$GAME_EXPORT" ]] || die "informe --game-export (ou NXRT_GAME_EXPORT)"
[[ -n "$RECOMP_BUILD" ]] || die "informe --recomp-build (ou NXRT_RECOMP_BUILD)"
[[ -n "$BOOST_ROOT" ]] || die "informe --boost-root (ou NXRT_BOOST_ROOT)"
[[ -d "$GAME_EXPORT" ]] || die "export não é um diretório: $GAME_EXPORT"
[[ -d "$RECOMP_BUILD" ]] || die "build do recompilador não é um diretório: $RECOMP_BUILD"
[[ -d "$BOOST_ROOT" ]] || die "Boost não é um diretório: $BOOST_ROOT"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die "-j deve ser um inteiro positivo"

[[ -f "$GAME_EXPORT/recomp_registration.c" ]] \
    || die "export inválido: $GAME_EXPORT/recomp_registration.c não existe"

GAME_LIBS=""
for rel in rtld/librecomp_static_rtld.a main/librecomp_static_main.a \
           sdk/librecomp_static_sdk.a main/librecomp_runtime_shared.a; do
    [[ -f "$RECOMP_BUILD/$rel" ]] || die "biblioteca ausente: $RECOMP_BUILD/$rel"
    GAME_LIBS+="${GAME_LIBS:+;}$RECOMP_BUILD/$rel"
done

compgen -G "$BOOST_ROOT/libs/*/include" >/dev/null \
    || die "Boost inválido: nenhum libs/*/include em $BOOST_ROOT"

echo "==> configurando em $BUILD_DIR"
cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DNXRT_GAME_EXPORT="$GAME_EXPORT" \
    -DNXRT_GAME_LIBS="$GAME_LIBS" \
    -DNXRT_BOOST_ROOT="$BOOST_ROOT"

echo "==> compilando nxrt"
cmake --build "$BUILD_DIR" --target nxrt -j "$JOBS"

echo "==> instalando em $PREFIX"
shopt -s nullglob
fonts=("$ROOT"/third_party/suyu/fonts/*.ttf)
((${#fonts[@]} > 0)) || die "nenhuma fonte .ttf encontrada em $ROOT/third_party/suyu/fonts"

# Cópias vão para arquivos temporários no mesmo diretório do destino e só entram no
# lugar com rename (atômico no mesmo sistema de arquivos). Falha ou interrupção
# nunca deixa arquivo parcial no destino, e uma instalação anterior fica intacta.
TMP_FILES=()
cleanup() { ((${#TMP_FILES[@]} == 0)) || rm -f -- "${TMP_FILES[@]}"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Espaço livre (KiB) no sistema de arquivos que conterá o destino.
free_kib() {
    local dir="$1"
    while [[ ! -d "$dir" ]]; do dir="$(dirname "$dir")"; done
    df -Pk -- "$dir" | awk 'NR==2 { print $4 }'
}

# atomic_install MODO ORIGEM DESTINO
atomic_install() {
    local mode="$1" src="$2" dest="$3" tmp
    tmp="$(mktemp "$dest.tmp.XXXXXX")" || die "não foi possível criar arquivo temporário junto de $dest"
    TMP_FILES+=("$tmp")
    cp -- "$src" "$tmp" || die "falha ao copiar $src para $tmp (nada foi instalado em $dest)"
    chmod "$mode" "$tmp"
    mv -f -- "$tmp" "$dest" || die "falha ao instalar $dest"
}

need_bytes="$(stat -c %s "$BUILD_DIR/nxrt")"
for font in "${fonts[@]}"; do
    need_bytes=$((need_bytes + $(stat -c %s "$font")))
done
# margem de 16 MiB; o temporário coexiste com o binário antigo, então conta o tamanho inteiro.
need_kib=$(((need_bytes + 1023) / 1024 + 16384))
avail_kib="$(free_kib "$PREFIX/bin")"
[[ "$avail_kib" =~ ^[0-9]+$ ]] || die "não foi possível medir o espaço livre em $PREFIX"
if ((avail_kib < need_kib)); then
    die "espaço insuficiente em $PREFIX: necessário $((need_kib / 1024)) MiB, livre $((avail_kib / 1024)) MiB (nada foi copiado)"
fi

install -d "$PREFIX/bin" "$PREFIX/bin/fonts"
atomic_install 0755 "$BUILD_DIR/nxrt" "$PREFIX/bin/nxrt"
# main.cpp procura fonts/ ao lado do executável; sem isso cairia no caminho do checkout.
for font in "${fonts[@]}"; do
    atomic_install 0644 "$font" "$PREFIX/bin/fonts/$(basename "$font")"
done

echo "pronto: $PREFIX/bin/nxrt"
echo "fontes: $PREFIX/bin/fonts"
echo "execute: $PREFIX/bin/nxrt --game <pasta-do-jogo> --user <pasta-de-saves>"
