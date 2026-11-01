# Teste do nxrt no Windows (4 GB de RAM, GPU Intel integrada)

Este roteiro prepara uma tentativa de configurar e compilar apenas `nxrt_core`, sem export real do jogo. Os comandos
são PowerShell 5.1 ou 7+, executados na raiz do repositório. Não rode o jogo neste roteiro.

O roteiro e a existência de caminhos `WIN32` no CMake não demonstram que o build funciona no Windows. No estado
registrado em `docs/INSTALL.md`, o PowerShell, o build, a instalação e o runtime ainda não foram exercitados em
Windows. Se a compilação de `nxrt_core` for executada com sucesso em uma máquina Windows, isso será evidência somente
para esse alvo, configuração e toolchain; não validará o link de `nxrt.exe`, o instalador completo, Vulkan em execução
ou o jogo.

---

## Passo 1: ferramentas do host e ajuda do instalador

### Instalar antes

- **PowerShell**: já vem no Windows (5.1). PowerShell 7 é opcional.
- **CMake** 3.20 ou mais novo, no `PATH`.
- **Ninja**, no `PATH`.
- **LLVM** com `clang` e `clang++` no `PATH` (o script confere os quatro: `cmake`, `ninja`, `clang`, `clang++`).

Também é necessário um Windows SDK/toolchain compatível para os headers e runtimes de destino; este requisito não é
fornecido nem instalado pelo script. O CMake do projeto aceita GCC/Clang (incluindo clang-cl), não `cl.exe` sem Clang.

### 1a. Ajuda do script

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1 -Help
```

**Deve aparecer:** o texto `uso: scripts\install.ps1 -GameExport DIR ...`, e o comando sai com código 0.

### 1b. Sem parâmetros: conferir o pré-requisito de ferramentas

```powershell
powershell -ExecutionPolicy Bypass -File scripts\install.ps1
echo "codigo: $LASTEXITCODE"
```

**Deve aparecer:** `erro: informe -GameExport (ou NXRT_GAME_EXPORT)` e `codigo: 1`.

Essa mensagem é esperada quando as ferramentas do script estão no PATH. Se aparecer outra coisa, pare e mande a saída:

- `erro: ferramenta ausente no PATH: ...`: falta instalar ou colocar no `PATH` a ferramenta citada.

### Mandar de volta (passo 1)

- saída de `-Help` (1a);
- saída completa de 1b, com `codigo:`;
- versões: `cmake --version`, `ninja --version`, `clang --version`, `clang++ --version`, `$PSVersionTable.PSVersion`.

---

## Passo 2: configuração CMake para o alvo `nxrt_core`

### Instalar antes (além do passo 1)

Estas dependências de desenvolvimento **não** são instaladas pelo `install.ps1`. CMake vai informar se faltar alguma:

- **fmt** com arquivo de configuração CMake (`find_package(fmt REQUIRED)`);
- **SDL2** de desenvolvimento, com `SDL2Config.cmake` (`find_package(SDL2 REQUIRED)`);
- **Vulkan SDK** (`find_package(Vulkan REQUIRED)` no Windows);
- **Boost** (árvore de fonte com `libs\*\include`). **É obrigatória para compilar** `shader_recompiler`, como medi no
  Linux (ver passo 3). Anote o caminho da pasta que contém `libs\`.
- **Windows SDK/UCRT** correspondentes à toolchain; sem eles, compilador/linker podem não encontrar headers ou runtimes
  do destino.

Vcpkg ou os instaladores oficiais servem. Anote o caminho de cada um.

### Export do jogo: use o stub

Você não tem o export grande (`exefs\` e as bibliotecas do jogo). Para configurar e compilar `nxrt_core` sem ele,
cria-se um **export de mentira**: uma pasta com um `recomp_registration.c` vazio. O CMake aceita isso porque o
`nxrt_core` não depende do export. O `nxrt` completo **não** pode ser linkado com ele, e isso é esperado.

```powershell
New-Item -ItemType Directory -Force "$env:TEMP\nxrt-stub\exefs" | Out-Null
Set-Content -LiteralPath "$env:TEMP\nxrt-stub\exefs\recomp_registration.c" -Value "/* stub: sem export do jogo */"
```

### Configuração com export stub

Na raiz do repositório. Troque `C:\caminho\boost-1.90.0` pela sua pasta do Boost:

```powershell
cmake -S . -B build-stub -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
  "-DNXRT_GAME_EXPORT=$env:TEMP\nxrt-stub\exefs" `
  "-DNXRT_GAME_LIBS=stub" `
  "-DNXRT_BOOST_ROOT=C:\caminho\boost-1.90.0" 2>&1 | Tee-Object cfg-stub.log
echo "codigo: $LASTEXITCODE"
```

`NXRT_GAME_LIBS=stub` não é um arquivo real. Ele só serve enquanto o `nxrt` não é linkado (passo 3 compila apenas
`nxrt_core`).

**Deve aparecer:** `-- Build files have been written to: ...build-stub` e `codigo: 0`.

Se falhar, a mensagem seguinte costuma indicar a dependência que faltou: `Could NOT find fmt`, `SDL2`, `Vulkan`
ou `NXRT_BOOST_ROOT`. Mande a saída inteira (`cfg-stub.log`).

### Mandar de volta (passo 2)

- arquivo `cfg-stub.log` inteiro;
- `codigo:` do comando;
- caminhos/versões das dependências configuradas (fmt, SDL2, Vulkan SDK, Boost e Windows SDK/toolchain).

---

## Passo 3: compilar `nxrt_core` sem o export grande

O alvo `nxrt` (o executável) não é compilado aqui: ele linka os módulos do jogo e o registro do export. O que este
passo mede é o restante do código, que é onde estão as mudanças de Windows da frente F (memória, threads,
fuso horário, arquivos, caminhos). `nxrt_core` e `shader_recompiler` não dependem do export.

### Comando

Com o mesmo `build-stub` do passo 2, uma compilação de cada vez. Comece com `-j 2`:

```powershell
cmake --build build-stub --target nxrt_core -j 2 2>&1 | Tee-Object build-stub.log
echo "codigo: $LASTEXITCODE"
```

**Deve aparecer:** a última linha `Linking CXX static library ...nxrt_core.lib` (ou `.a`), `codigo: 0`.

Se o computador travar ou ficar sem memória, interrompa com Ctrl+C, reduza para `-j 1` e rode de novo. Se a compilação
falhar, **esse é um resultado do teste**, não um erro do roteiro: o código de Windows nunca foi compilado. Mande
o log.

Para ver só os erros, se o log for grande:

```powershell
Select-String -Path build-stub.log -Pattern 'error' | Select-Object -First 40
```

### Mandar de volta (passo 3)

- `build-stub.log` inteiro (ou, se for grande, o final e as linhas de `error`);
- `codigo:`;
- memória e vídeo da máquina:

```powershell
Get-CimInstance Win32_OperatingSystem | Select-Object TotalVisibleMemorySize, FreePhysicalMemory
Get-CimInstance Win32_VideoController | Select-Object Name, AdapterRAM
```

- espaço livre no disco do repositório: `Get-PSDrive -Name (Get-Location).Drive.Name`.

---

## Build completo e instalação

O stub acima não permite linkar `nxrt.exe`. O fluxo completo do `scripts\install.ps1` precisa do export pessoal em
`exefs\` com `recomp_registration.c` e das quatro bibliotecas estáticas do mesmo build do recompilador:

- `rtld\librecomp_static_rtld`;
- `main\librecomp_static_main`;
- `sdk\librecomp_static_sdk`;
- `main\librecomp_runtime_shared`.

O script aceita cada biblioteca como `.a` ou `.lib`, além do Boost. A configuração e a compilação completas continuam
sem validação Windows; executar o instalador e iniciar o runtime são passos posteriores, também não verificados.

## O que não cabe em 4 GB (ou não deve ser feito aqui)

| Item | Situação | Dado |
| --- | --- | --- |
| Gerar o export do recompilador (`exefs\`, `recomp\`) | Não cabe. Faça em outra máquina e traga só `exefs\` e os `.lib`/`.a` das bibliotecas. | não medido aqui |
| Linkar `nxrt.exe` com os módulos do jogo | Não cabe. O `INSTALL.md` estima ~2,3 GB de binário; o link consome muita RAM e disco. | estimativa do `INSTALL.md` |
| `build-install\` completo, com o export real | Ocupa ~2,4 GB. | medido no build Linux de `build-install/` |
| `nxrt_core` com o stub | ~148 MB. | medido no Linux, `stub-check/` |
| Árvore do Boost | ~691 MB para o Boost 1.90.0 inteiro (cache do CPM). Só `libs\*\include` é usado. | medido no Linux |
| Rodar o jogo | Não cabe. O F2 estima que o jogo compromete até ~3,3 GiB de memória privada, mais do que os 4 GB físicos com o sistema. Além disso, a Intel integrada compartilha a RAM. | estimativa, do relatório F2 |
| Compilação em paralelo acima de `-j 2` | Pode faltar RAM nos arquivos grandes do `shader_recompiler`. O instalador agora limita o padrão a até 2 tarefas; um usuário pode reduzir com `-Jobs 1`. | risco de memória; não medido no Windows |
| Desligar o arquivo de paginação | Não desligue. Com 4 GB, o Windows depende dele. | — |

Se o passo 3 passar em Windows, o próximo passo é obter o export e as quatro bibliotecas em outra máquina e avaliar o
build completo em uma máquina com mais memória. Isso ainda exigirá validar instalação e execução em Windows real.
