# Testando o port nativo (alpha)

Esta página diz o que foi testado, o que não foi e como você pode ajudar. 🇬🇧 [English](TESTING.md)

## O que é

Um **port nativo não oficial para PC** de *Tomodachi Life: Living the Dream*, gerado **no seu computador** a partir da **sua própria
cópia** do jogo (`.nsp`, versão 1.0.0) e do seu `prod.keys`. O código ARM do jogo é recompilado estaticamente para x86-64 e roda no
**nxrt**, o runtime deste projeto, com renderizador NVN → Vulkan próprio. Parte dos serviços do sistema Switch ainda vem de código
derivado do suyu. Nada do jogo, do firmware nem das suas chaves é distribuído.

## Onde foi testado

| Item | Testado |
|---|---|
| Sistema | Linux: EndeavourOS (Arch), kernel 7.x |
| CPU / RAM | Intel i5-13450HX, 16 GB de RAM + 16 GB de swap |
| GPU | NVIDIA GeForce RTX 4050 Laptop (driver proprietário 615.x), Vulkan 1.4 |
| Jogo | Tomodachi Life: Living the Dream **1.0.0, sem atualizações** |
| Por quem | uma pessoa (o mantenedor) |

## O que funciona (observado pelo mantenedor)

- Início, título, criação de perfil, a ilha, criação de Mii, o mercado e as telas de Residents.
- 30 FPS na ilha (o limite é 30). Quedas breves para cerca de 23 FPS acontecem quando uma cena nova carrega.
- O editor de facepaint: a **primeira** abertura leva cerca de 40 s (prepara um cache em `~/.cache/nxrt`); as seguintes levam poucos segundos.
- Os saves ficam em `local/user-nxrt` e os dados originais do jogo nunca são modificados.

## O que NÃO foi testado (ajuda bem-vinda)

- **GPUs AMD e Intel**, e GPUs NVIDIA diferentes da RTX 4050 (o decodificador de texturas na GPU e o envio assíncrono são novos).
- **Outras distribuições** (Fedora, Ubuntu, Debian, derivadas do Arch, SteamOS) e sessões somente Wayland.
- **Windows**: ainda não suportado (o port nativo é só para Linux por enquanto).
- Sessões longas, partes do jogo que ninguém jogou ainda e saves de um console de verdade.
- Uma instalação limpa a partir do `.nsp` numa máquina nova (a build do mantenedor foi feita passo a passo em um laboratório).

## Como testar

1. Confira os requisitos no [README](../README.pt-BR.md): Linux, 16 GB de RAM + 8 GB de swap, **35 GB livres**, GPU com Vulkan.
2. Rode `./install.sh` e depois `./play.sh`.
3. Jogue um pouco. Experimente: criar um Mii, andar pela ilha, abrir o mercado, o editor de facepaint (duas vezes) e mudar de cena.
4. Opcional: `NXRT_ASTC_GPU=0 ./play.sh` desliga o decodificador de texturas na GPU, e `NXRT_NO_ASYNC_GRAPHICS=1 ./play.sh` desliga o envio assíncrono. Se um problema some com uma dessas opções, conte no relato: isso aponta direto para a causa.
5. Para ver a velocidade, procure as linhas `vi: ... fps` em `local/logs/play-nxrt-*.log`.

## Como relatar um problema

Abra uma [issue](https://github.com/RussoPhone/tomodachi-ltd-recomp/issues/new?template=install-or-play-problem.md) com:

- **Problema na instalação:** rode `./install.sh --report` e anexe o zip. Ele tem logs e um resumo do sistema (distribuição, GPU, driver, versões das ferramentas). **Não** tem dados do jogo nem chaves, e as pastas pessoais viram `~`.
- **Problema ao jogar:** anexe o `local/logs/play-nxrt-*.log` daquela execução, diga o que estava fazendo e, se a imagem estiver errada, uma captura de tela.
- Informe a sua GPU e o driver (`./play.sh --nogui --diagnose` mostra), e se `./play.sh --nogui --safe` ou desligar as opções do passo 4 muda alguma coisa.
- Veja também [COMPATIBILITY.pt-BR.md](COMPATIBILITY.pt-BR.md) e [TROUBLESHOOTING.pt-BR.md](TROUBLESHOOTING.pt-BR.md).

Por favor, **não** publique chaves, firmware, o `.nsp` nem qualquer cópia do jogo.

## Limitações conhecidas

- **O suporte ao mouse é experimental e esquisito** em alguns menus (o criador de Mii em especial); funciona melhor na ilha. Teclado e controle funcionam em tudo. Veja [CONTROLS.pt-BR.md](CONTROLS.pt-BR.md).
- Limite de 30 FPS; quedas enquanto cenas carregam.
- A primeira abertura do facepaint é lenta (~40 s).
- As opções de tela cheia, tamanho da janela e idioma (e o launcher) são novas: só foram testadas na máquina do mantenedor (texto em japonês e inglês e um tamanho de janela personalizado foram conferidos); não existe um menu de configurações dentro do jogo.
- Alpha: espere travamentos e falhas gráficas, e não confie na única cópia de um save importante (`local/user-nxrt` é uma pasta separada, mas faça backup).
