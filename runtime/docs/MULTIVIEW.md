# Multiview (replay de quadros do nxrt)
> Nome: **multiview**. Antes de 2026-10-18 chamava-se "replay" (binário `nxrt-replay`, pasta `tools/replay`). Os módulos internos (`src/replay/`) e as variáveis `NXRT_CAPTURE_*` mantêm o nome antigo para não quebrar capturas existentes.


Ferramenta para repetir **um quadro já gravado** em segundos, sem rodar o jogo de novo. Serve para testar mudanças no
renderer (NVN → Vulkan) rapidamente e comparar o resultado com a imagem original.

## Para que serve
Chegar a um ponto do jogo leva minutos (a ilha, por exemplo, ~19 min). A captura grava as chamadas NVN e a memória do
jogo daquele instante; o `nxrt-multiview` redesenha só aquilo, offscreen, em poucos segundos.

## Como funciona
1. **Gravador** (`src/replay/capturing_renderer.cpp`): um decorador do renderer, ativado por variável de ambiente.
   Guarda o `Model`, os eventos (`Execute`/`Present`) e as páginas de memória que o renderer leu (4 KiB, deduplicadas
   por hash). Formato em `src/replay/capture_format.{h,cpp}`.
2. **Reprodutor** (`tools/multiview/main.cpp`, alvo `nxrt-multiview`): remonta a memória e reexecuta os eventos no renderer real.
3. **Comparador**: `comparar_regioes.py` (fora do nxrt, em `TOOLS/`) mede a % de pixels diferentes por região.

## Uso
```
# 1. gravar (roda o jogo ate o quadro alvo e para)
NXRT=build/nxrt GAME=<jogo> SAVE=<save descartavel> TARGET=1400 CAPTURE=1398:3 \
  tools/multiview/capturar-quadro.sh <saida>

# 2. repetir e comparar com a imagem original gravada na mesma rodada
NXRT_MULTIVIEW=build/nxrt-multiview COMPARADOR=<comparar_regioes.py> TARGET=1400 \
  tools/multiview/repetir.sh <saida>/captura <saida>/dump/frame-1400.ppm
```
Variáveis do gravador: `NXRT_CAPTURE_FRAMES=primeiro:quantidade` (o quadro anterior ao primeiro serve de pré-roll) e
`NXRT_CAPTURE_DIR`. Para trocar o dispositivo Vulkan, use `VK_DRIVER_FILES`.


## Detector de regressão (uso no dia a dia)
A régua de produção é **repetição × base**, não × original. O replay é determinístico (0% entre repetições); o jogo
original não é. Então, depois de mexer no renderer, qualquer diferença contra a base vem da sua mudança.

```
# conjunto = pasta com pontos.conf (nome|quadro|pasta-da-captura|limite-%) e as <nome>.base.ppm
NXRT_MULTIVIEW=build/nxrt-multiview COMPARADOR=<comparar_regioes.py> tools/multiview/rodar-todos.sh <conjunto>
# primeira vez ou mudanca aprovada:  ... rodar-todos.sh <conjunto> --atualizar
```
Exemplo medido (Intel, 3 pontos, 16 s no total): sem mudança, 0,00% nos três. Com `NXRT_LEGACY_VIEWPORT=1` (viewport
sem a inversão de Y): título 0,00%, quarto 0,01%, **ilha 35,10%, ACIMA DO LIMITE**. Ou seja, pega a mudança onde ela importa.
Pontos que falham deixam `<nome>.ultima.ppm` na pasta do conjunto para você olhar.

Escolha dos pontos: título (cena parada), quarto do Mii (personagem, rosto, balão) e ilha (água, terreno, HUD). Para
proteger outra cena, grave-a com `capturar-quadro.sh` e acrescente uma linha em `pontos.conf`.

## Regras
- **Capturas contêm memória do jogo: nunca vão para o git, nem para issues.** Tamanho típico: 300–800 MB.
- Use sempre uma **cópia descartável do save** (o script copia).
- Um jogo por vez.

## Como ler o resultado
O próprio jogo não repete o mesmo quadro: ondas e animações de texto variam. Medido na ilha e no título, dois originais
diferem entre si 3–6% (tolerância 8 por canal). O critério correto é, portanto, **"o replay fica dentro do ruído de dois
originais"**, não "0%". Duas repetições do mesmo replay devem ser idênticas.

| Cena | Replay × original | Original × original | Replay × replay |
|---|---:|---:|---:|
| Título (q1400) | 0,74% (3,58% contra outro original) | 3,31% | 0% (idênticos) |
| Ilha (q11000) | ~44% | 3–6% | ~0,003% |

## Limites conhecidos
- Em cenas complexas (ilha, água) o replay ainda difere muito do original: o formato não guarda o estado interno da GPU
  (conteúdo anterior de render targets, bindings e caches herdados, contadores de `ReportCounter`). Veja `VALIDACAO.md`
  e `S4-STATUS.md`. **Não use a ilha como referência de regressão até isso ser resolvido.**
- Atrelado à interface do renderer do nxrt; não serve para emuladores.
