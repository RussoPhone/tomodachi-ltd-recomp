# Solução de problemas e relatório de bug (nxrt)

Para quem está testando o port. Para instalar, veja `docs/INSTALL.md`. Este documento só descreve o que já foi
observado e registrado no repositório; o que não foi verificado está marcado como tal.

## 1. Capturar o log

O nxrt escreve o log **só em stderr**, uma linha por evento, no formato `[  12.345] info  texto`
(tempo em segundos desde o início, nível, mensagem). Não existe arquivo de log próprio: redirecione você mesmo.

```bash
NXRT_LOG=info ./nxrt --game <pasta do jogo instalado> --user <pasta do save> 2>&1 | tee nxrt.log
```

- `tee` mostra na tela e grava no arquivo ao mesmo tempo. Sem `tee`, use `2> nxrt.log`.
- **`NXRT_LOG`** aceita `trace`, `debug`, `info`, `warning`, `error`. Qualquer outro valor (ou ausente) vale `info`.
  Use `info` por padrão; `debug` e `trace` geram logs enormes e deixam o jogo mais lento, só peça/use quando o
  mantenedor pedir.
- Variáveis úteis, todas opcionais: `NXRT_RENDER_STATS=1` (desenhos e pulos por quadro), `NXRT_NVN_STATS=1`,
  `NXRT_SVC_STATS=1`, `NXRT_IPC_STATS=1` (contagem de serviços/IPC), `NXRT_SCREENSHOT` e
  `NXRT_DUMP_EVERY=N NXRT_DUMP_DIR=<pasta>` (salvam quadros; `--offscreen` pode não gerar o arquivo, ver o relatório
  AGENTE-D). Não são necessárias para um relatório simples.
- Para um teste curto e repetível, `tools/teste-60s.sh` roda o jogo por 60 s com cópia do save e resume o log
  (`tools/triagem-log.py LOG`). Só é útil dentro do repositório de desenvolvimento.

### Use uma cópia do save

Passe em `--user` uma **cópia** do save, nunca o original: o jogo grava `Map.sav`, `Mii.sav` e `Player.sav` nessa pasta.

### Caso o programa caia

O nxrt é o jogo recompilado rodando em processo único. Uma queda aparece no log como `guest crashed` ou
`guest read/write/fetch fault ... fault at 0x...` seguida de backtrace (`main+0x...`). Guarde o log inteiro: o
endereço e o tempo da queda (ex.: ~20 s após iniciar) são o dado mais valioso. Queda por volta de 20 s já foi
reproduzida antes da tela de nome; informe o tempo mesmo assim.

## 2. Erros conhecidos de instalação

### Dependências de compilação

O instalador não tenta adivinhar pacotes de distribuição nem instalar dependências. Se a configuração do CMake falhar,
instale as dependências de desenvolvimento que ela indicar e rode `scripts/install.sh` novamente. O projeto usa CMake
para localizar `fmt`, SDL2, Threads e o loader Vulkan; o shader recompiler também precisa dos headers Boost indicados por
`--boost-root`. FFmpeg e Qt6GuiPrivate não são dependências deste build.

### Windows: erro menciona `PATCH_EXE`

O `scripts/install.ps1` atual não usa `PATCH_EXE` nem exige `patch.exe`. Se um erro externo mencionar essa variável ou
arquivo, identifique o comando e o caminho do arquivo que emitiu a mensagem; não restaure o pacote como solução automática.

Se o `.ps1` falhar por política de execução, use
`powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -GameExport ... -RecompBuild ... -BoostRoot ...`.
Qualquer outro erro no Windows é novidade: mande o relatório completo (seção 4).

### Outros erros do instalador

Entradas ausentes (`exefs/`, `recomp`, Boost), pouco espaço em disco (o binário tem ~2,3 GB e o build ~2,4 GB) e
ferramentas ausentes (`cmake`, `ninja`, `clang`, `clang++`) são detectados antes da compilação com mensagem clara.
O build leva de 15 a 30 minutos. Cópia interrompida não corrompe uma instalação anterior.

## 3. Teclado e entrada de texto (issue #6)

### Controles do jogo

`X`=A, `Z`=B, `S`=X, `A`=Y, `Q`/`E`=L/R, `1`/`3`=ZL/ZR, `Enter`=+, `Backspace`=−, setas=direcional, `I J K L`=analógico.
Gamepad SDL também funciona. O título da janela mostra o fps.

### Digitar nomes (teclado do jogo)

Estado conhecido (relatório AGENTE-D), com as limitações:

- O texto que você digita na janela do nxrt é guardado em um buffer (máx. 64 caracteres, expira após 10 s sem
  digitar) e usado quando o jogo abre o teclado. Se o buffer estiver vazio, o nome vem de `NXRT_SWKBD_TEXT`,
  depois `NXRT_USER_NAME`, senão `Nxrt`. Para fixar o nome: `NXRT_USER_NAME="Fulano" ./nxrt ...`.
- **O texto precisa ser digitado antes de o jogo abrir o teclado**, e as teclas de jogo (`Z`, `X`...) também entram
  no buffer se estiverem a menos de 10 s do momento em que o teclado abre. Se o nome vier com letras estranhas,
  é isso.
- O jogo usa apenas o teclado **inline** (ele mesmo desenha). O backend inline existe, mas **ainda não foi provado
  em jogo**: nos testes automáticos o teclado nunca foi aberto em 60–120 s. Não há garantia de que digitar
  funcione; relate o que aconteceu.
- Patch 0009 do suyu (caixa de texto nativa, no repositório `tomodachi-ltd-recomp`): no Linux usa `zenity` ou
  `kdialog`; no Windows usa uma `InputBox` do PowerShell. Se nenhum diálogo aparecer no Linux, instale `zenity` ou
  `kdialog` e confira com `which zenity kdialog`. Problemas conhecidos desse patch: no Windows acentos/kana podem
  chegar como caracteres errados; no Linux `&` e `<` no texto do jogo podem quebrar o rótulo do `zenity`. Texto vazio
  conta como cancelar.

## 4. O que mandar num relatório de bug

Cole tudo isto na issue (ou anexe); sem isso o relatório costuma ser inconclusivo:

1. **Sistema:** distribuição e versão (`cat /etc/os-release`), kernel (`uname -r`), CPU/RAM, GPU e driver
   (`vulkaninfo --summary | head -30`), ambiente (X11/Wayland).
2. **Versão do nxrt:** `git rev-parse HEAD` do repositório e se há alterações locais (`git status --short`).
3. **Como instalou:** comando completo do `install.sh`/`install.ps1` e a saída. Se falhou, a saída **inteira**,
   não só a última linha.
4. **Como executou:** comando exato e variáveis `NXRT_*` usadas, e se foi janela, `--headless` ou `--offscreen`.
5. **O que esperava e o que aconteceu**, com o tempo aproximado desde o início (queda aos 20 s? travou na tela
   de nome?). Diga também se `--user` apontava para um save novo ou existente (cópia).
6. **O log:** arquivo gerado com `2>&1 | tee` (seção 1), em `info`. Se o arquivo for grande, anexe os primeiros 200
   e as últimas 200 linhas e guarde o inteiro.
7. **Se caiu:** as linhas `guest crashed` / `fault at` com o backtrace, e o tempo da queda.
8. **Se envolve teclado:** o que digitou, quando (antes/depois de o jogo abrir o teclado) e se `zenity`/`kdialog`
   está instalado.
9. **Se for visual:** uma captura de tela da janela.

Não envie o binário do nxrt, o export do jogo, chaves, `prod.keys` ou qualquer arquivo do jogo: o binário embute
código do jogo e não deve ser distribuído.
