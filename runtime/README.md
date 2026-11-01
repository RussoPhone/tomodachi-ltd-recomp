# nxrt

Runtime nativo para executar o export local do recompilador. A instalação compila o executável na máquina do usuário;
ele incorpora código do jogo e não deve ser redistribuído.

## Instalação

O instalador Linux usa um export próprio (`exefs/`), as bibliotecas estáticas do build `recomp/` e uma árvore de fontes
do Boost. Ele não baixa arquivos nem instala dependências do sistema. Exemplo:

```bash
scripts/install.sh \
  --game-export <pasta>/exefs \
  --recomp-build <pasta>/recomp \
  --boost-root <pasta>/boost
```

Por padrão, instala em `~/.local/share/nxrt`. Veja [o guia completo de instalação](docs/INSTALL.md) para dependências,
variáveis de ambiente, opções e limitações. O instalador e o runtime Windows continuam experimentais e não foram
executados em Windows.

## Executar

```bash
~/.local/share/nxrt/bin/nxrt --game <pasta-do-jogo> --user <pasta-de-saves>
```

Use uma cópia dos saves. O percurso completo do jogo e a persistência da ilha ainda não foram demonstrados; o estado
atual e os limites conhecidos estão descritos no [guia de instalação](docs/INSTALL.md).
