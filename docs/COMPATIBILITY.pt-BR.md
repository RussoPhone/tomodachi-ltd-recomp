# Compatibilidade

🇬🇧 [English](COMPATIBILITY.md)

O que foi testado em qual hardware. **Só a primeira linha foi testada pelo mantenedor**; todas as outras são lacunas que você pode preencher.
Adicione a sua com um [relato de compatibilidade](https://github.com/RussoPhone/tomodachi-ltd-recomp/issues/new?template=compatibility-report.md).

## Testado

| Situação | GPU / driver | Sistema | Resultado | Relatado por |
|---|---|---|---|---|
| ✅ Funciona | NVIDIA GeForce RTX 4050 Laptop, proprietário 615.x, Vulkan 1.4 | EndeavourOS (Arch), kernel 7.x, sessão Wayland via XWayland | Instalação completa; 30 FPS na ilha; quedas breves para ~23 FPS ao carregar cenas | mantenedor |

## Windows: primeiro passo feito

O runtime `nxrt` agora **compila e linka no Windows** (GitHub Actions, `windows-2022`, clang, contra substitutos do lado do jogo; veja
`.github/workflows/nxrt-windows.yml`). Isso é tudo que isso prova: nada foi executado no Windows, a compilação do lado do jogo (o recompilador
e o instalador) continua só para Linux, e não há instalador para Windows. O Windows **ainda não é suportado**.

## Ainda não testado (ajuda bem-vinda)

| O quê | Por que importa |
|---|---|
| GPUs AMD (Mesa RADV) | A mais pedida; o decodificador de texturas na GPU e o envio assíncrono são código novo |
| GPUs Intel (Mesa ANV) | GPUs integradas podem ser lentas demais para 30 FPS; não sabemos |
| Outras GPUs NVIDIA (séries GTX 10/16, RTX 20/30/40) | Gerações diferentes, mesma família de driver |
| Ubuntu, Debian, Fedora, openSUSE, CachyOS, Manjaro | Os comandos de instalação estão escritos, mas não verificados |
| SteamOS / Steam Deck | Não testado; espaço em disco e tempo de compilação podem ser um problema. O sistema é somente leitura: compile e jogue dentro de um distrobox (veja abaixo) |
| Sessões só X11, KDE, GNOME e outros desktops | Janela e tela cheia |
| Notebooks híbridos (NVIDIA + Intel/AMD) | O port escolhe a GPU dedicada primeiro; veja `GPU:` em `./play.sh --nogui --diagnose` |

## Como verificar a sua máquina

```bash
./play.sh --nogui --diagnose      # GPU, versão do Vulkan, memória, sessão, ferramentas que faltam
./play.sh --nogui --safe          # caminho gráfico conservador, se a imagem estiver errada
```

No launcher: **Check my computer** e a caixa **Safe mode** fazem o mesmo.

## O que o port precisa

- Linux com driver de GPU Vulkan 1.2 (ou mais novo). Um dispositivo Vulkan só de software (llvmpipe) é lento demais.
- 16 GB de RAM e 8 GB de swap para instalar (compilar consome muita memória), ~27 GB de disco depois de instalado (recomendado ter 35 GB livres).
- A sua cópia do jogo (`.nsp`, versão 1.0.0) e o `prod.keys`.

## O que o modo seguro muda

`NXRT_SAFE_MODE=1` (ou `--safe`) desliga os caminhos mais novos e rápidos: envio assíncrono de gráficos, descarga assíncrona de compute e o
decodificador de texturas na GPU (as texturas passam a ser decodificadas na CPU). É mais lento e serve para responder uma pergunta: *o
problema some?* Se some, diga isso no relato: aponta direto para a causa.

## SteamOS / Steam Deck

O SteamOS mantém o sistema somente leitura, então as ferramentas de compilação não podem ser instaladas nele. Use um contêiner distrobox:

```bash
distrobox create --name tomodachi --image archlinux:latest
distrobox enter tomodachi
```

Dentro dele, siga o "Passo 1" do README e rode `./install.sh` e `./play.sh` no mesmo contêiner. O Deck tem no máximo 16 GB de RAM e pouco espaço livre: a instalação pede cerca de 35 GB livres e bastante swap, e a compilação pode levar horas. Se não couber, compile em outro PC e copie a pasta `local/`.
