# Instalação do nxrt (Linux, v1)

O nxrt não é distribuído como binário pronto. O executável embute os módulos recompilados do jogo, então ele é
compilado na sua máquina, a partir do export que você mesmo gerou. Não envie o binário gerado para outras pessoas.

## Pré-requisitos

- `cmake`, `ninja`, `clang` e `clang++` no PATH.
- Dependências de desenvolvimento para `fmt`, SDL2 e o loader Vulkan (`libvulkan`). Os headers Vulkan já estão no
  repositório; nomes dos pacotes de sistema variam por distribuição.
- Export do recompilador (pasta `exefs/` com `recomp_registration.c`).
- Diretório `recomp` do build estático, com `rtld/`, `main/` e `sdk/` contendo as bibliotecas `.a`.
- Árvore de fonte do Boost (ex.: `boost-1.90.0`), com `libs/*/include`.

O script não instala dependências do sistema nem baixa arquivos. Prepare esses requisitos e gere o export e as
bibliotecas por meios legítimos antes de executar a instalação.

## Instalar

```bash
scripts/install.sh \
  --game-export <pasta>/exefs \
  --recomp-build <pasta>/recomp \
  --boost-root <pasta>/boost-1.90.0
```

Os mesmos caminhos podem vir de variáveis de ambiente: `NXRT_GAME_EXPORT`, `NXRT_RECOMP_BUILD` e `NXRT_BOOST_ROOT`.

Opções: `--prefix` (padrão `~/.local/share/nxrt`), `--build-dir` (padrão `build-install/` na raiz do repo) e `-j N`.

O script confere as entradas antes de compilar e para com uma mensagem clara se alguma faltar.
O build fica em `build-install/` por padrão. O executável e as fontes livres substitutas usadas por `pl:u` são instalados
em `~/.local/share/nxrt/bin/`; use `--prefix` para escolher outro destino. O instalador não remove conteúdo já existente
nesse diretório.

A cópia é segura: antes de copiar o script confere se há espaço livre no destino (binário + fontes + 16 MiB de margem; o
binário tem ~2,3 GB) e para com erro claro se faltar. Cada arquivo é copiado para um temporário no mesmo diretório e
só então renomeado; se a cópia falhar ou for interrompida, o temporário é removido e uma instalação anterior continua
intacta. O espaço do próprio build (`build-install/`, ~2,4 GB) é por sua conta.

## Windows (experimental, não testado)

`scripts/install.ps1` existe e replica o fluxo do `install.sh` em PowerShell 5.1 ou 7+.

Parâmetros (todos vindos do próprio script):

| Parâmetro | Equivalente Linux | Padrão |
| --- | --- | --- |
| `-GameExport` | `--game-export` | `NXRT_GAME_EXPORT` |
| `-RecompBuild` | `--recomp-build` | `NXRT_RECOMP_BUILD` |
| `-BoostRoot` | `--boost-root` | `NXRT_BOOST_ROOT` |
| `-Prefix` | `--prefix` | `%LOCALAPPDATA%\nxrt` |
| `-BuildDir` | `--build-dir` | `build-install\` na raiz do repo |
| `-Jobs` | `-j` | até 2 tarefas paralelas (reduz uso de RAM) |
| `-Help` | `--help` | mostra o uso e sai |

Paridade com o `install.sh`:

- Confere ferramentas (`cmake`, `ninja`, `clang`, `clang++`) e entradas antes de compilar; aceita bibliotecas `.a` ou `.lib`.
- Limita o paralelismo padrão a até 2 tarefas para reduzir picos de RAM; `-Jobs`/`-j` permite ajustar manualmente.
- Checagem de espaço livre: binário + fontes + 16 MiB de margem; para com erro claro, sem copiar nada, se faltar.
- Cópia segura: cada arquivo vai para um temporário no mesmo diretório e só então é renomeado
  (`File.Replace` ou `File.Move`); em falha o temporário é removido e a instalação anterior fica intacta.
- Instala `nxrt.exe` e `fonts\*.ttf` em `<Prefix>\bin\`.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 `
  -GameExport <pasta>\exefs -RecompBuild <pasta>\recomp -BoostRoot <pasta>\boost-1.90.0
```

### O que não foi testado

Em 2026-10-11, `scripts/install.sh` foi executado em Linux com o export e as bibliotecas reais, `--prefix` em `/tmp` e
`--build-dir build-native-clean`. O build limpo concluiu `[225/225]`, o binário foi instalado junto das seis fontes e
seu SHA-256 coincidiu com `build-native-clean/nxrt`. O executável instalado rodou em janela por 110,8 s; a captura
q3300 mostra a orientação “Not sure what sort of Miis to create?” com texto legível e a triagem não encontrou falha,
fault ou crash. Os três saves descartáveis mantiveram os hashes do baseline. Isso valida compilação limpa e instalação
Linux com os insumos reais, além da inicialização do jogo até essa tela; não valida Windows nem o percurso completo.

O script PowerShell **não foi executado**: não havia `pwsh` nem Windows na máquina de desenvolvimento, só revisão
estática (sintaxe e comparação linha a linha com o `install.sh`). Em particular, nada disto foi exercitado:

- a execução em PowerShell 5.1 e em 7+ (inclusive a política de execução e a codificação do arquivo, que tem BOM);
- o build com `clang`/`ninja` no Windows e o uso de bibliotecas `.lib`;
- a medição de espaço livre (`DriveInfo`); em caminho UNC (`\\servidor\...`) o script não consegue medir e aborta;
- o rename atômico (`File.Replace`) sobre uma instalação existente, e o cenário de falta de espaço.

O `CMakeLists.txt` contém caminhos condicionais de Windows, mas o build e a execução do runtime nunca foram verificados
em Windows. O roteiro em `docs/TESTE-WINDOWS.md` cobre apenas a configuração com um export stub e a compilação de
`nxrt_core`; isso não valida `nxrt.exe`, a instalação nem o jogo.

### Comando de checagem

Para ver o uso sem compilar nem copiar nada (só valida que o script carrega e os parâmetros):

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Help
```

Checagem de sintaxe sem executar (em qualquer máquina com `pwsh`, inclusive Linux):

```bash
pwsh -NoProfile -Command '$e=$null; [void][System.Management.Automation.Language.Parser]::ParseFile("scripts/install.ps1",[ref]$null,[ref]$e); if($e){$e; exit 1}else{"sintaxe ok"}'
```

Se alguém rodar o script de verdade no Windows, abra uma issue com o resultado.

## Executar

```bash
~/.local/share/nxrt/bin/nxrt --game <pasta-do-jogo> --user <pasta-de-saves>
```

`--game` aponta para a instalação do jogo e `--user` para a pasta de saves. Use uma cópia dos saves, não o original.

O código de região padrão vem do território de `LC_ALL`, `LC_MESSAGES` ou `LANG`. Para escolher explicitamente o
valor esperado por `set::GetRegionCode`, defina `NXRT_SYSTEM_REGION` como `Japan`, `USA`, `Europe`, `Australia`,
`HongKongTaiwanKorea` ou `China`.

## Limitações conhecidas

- **Atualizado em 2026-10-11:** Linux é a única plataforma executada no runtime. O `install.sh` compilou do zero com os
  insumos reais e o binário instalado iniciou em janela até a orientação de criação de Miis; o percurso completo não foi
  demonstrado.
- Janela, cenas do jogo, texto de UI, efeitos e música foram observados em execuções Linux. Isso não valida o percurso
  completo nem a estabilidade depois da confirmação de região.
- O port ainda não comprovou criação de ilha nem persistência do save. Toques no formulário “Island name” não o
  alteraram visualmente; a captura instalada posterior mostra uma tela de orientação para criar Miis, sem prova de
  avanço persistido.
- Windows continua experimental: `install.ps1`, build do runtime e jogo não foram executados em Windows. Veja
  `docs/TESTE-WINDOWS.md`.
- Esta instalação não inclui o jogo. Cada usuário precisa fornecer o próprio export e os arquivos locais; não distribua
  o binário compilado com código do jogo.
