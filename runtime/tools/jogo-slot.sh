#!/bin/bash
# Roda um comando segurando um de dois slots de jogo (regra: no máximo dois jogos ao mesmo tempo).
# Uso: tools/jogo-slot.sh <comando> [args...]
# Tenta os slots 1 e 2; se os dois estiverem ocupados, tenta de novo a cada segundo,
# pegando o primeiro que liberar. O lock fica com o processo e é liberado quando ele termina.
while true; do
    for s in 1 2; do
        exec 9>"/tmp/nxrt-game-$s.lock"
        if flock -n 9; then
            exec "$@"
        fi
    done
    exec 9>&-
    sleep 1
done
