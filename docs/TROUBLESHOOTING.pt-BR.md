# Solução de problemas

🇬🇧 [English](TROUBLESHOOTING.md)

Primeiro rode `./play.sh --nogui --diagnose`: ele confere a GPU, o Vulkan, a memória, a sessão e as ferramentas que faltam, e diz o que
corrigir. Seja o que for que você tente, um [relatório](TESTING.pt-BR.md#como-relatar-um-problema) (`./install.sh --report`, ou **Create bug
report** no launcher) é o que nos permite ajudar.

## Instalando

| Problema | O que fazer |
|---|---|
| "Missing software" | Rode de novo o comando do Passo 1 do README; a mensagem lista o que falta. |
| Falta de memória ao compilar | São necessários 16 GB de RAM + 8 GB de swap. Feche outros programas; o instalador continua de onde parou. |
| "Different version than the supported one" | O seu jogo tem atualização ou é outra versão. Só a 1.0.0 (sem atualizações) funciona. |
| O passo 7 diz que precisa de sessão gráfica | Rode o instalador num terminal dentro do desktop, não por SSH. |
| Uma janela da ferramenta fica aberta no passo 7 | Ela deveria fechar sozinha em segundos (é a ferramenta de extração, não o jogo). Se não fechar, envie o relatório; é um problema aberto conhecido (#13, #16). |
| Falta de espaço em disco | A instalação usa ~27 GB (recomendado 35 GB livres). Dá para apagar `local/` e recomeçar. |
| A instalação está muito lenta | Reserve de 1h30 a 2 horas numa máquina de 16 threads. Evite atualizações do sistema enquanto compila. |
| Qualquer outra coisa | `./install.sh --report` e anexe o zip numa issue. |

## Jogando

| Problema | O que fazer |
|---|---|
| O launcher não abre | Instale o Tk (`python3-tk` / `tk`), ou inicie com `./play.sh --nogui`. |
| "No Vulkan device" ou janela preta | Instale o driver Vulkan da sua GPU (o comando de diagnóstico mostra o pacote da sua distribuição). |
| FPS muito baixo | Em notebook com duas GPUs, garanta que a dedicada está em uso (`DRI_PRIME=1 ./play.sh` em AMD/Intel, `prime-run ./play.sh` em NVIDIA). Um dispositivo Vulkan de software (llvmpipe) é lento demais. |
| Texturas estranhas, faltando ou com cores erradas | Experimente o **Safe mode** (`./play.sh --nogui --safe`). Se resolver, diga isso no relato. |
| Trava ao iniciar | Envie o `local/logs/play-nxrt-*.log` (as primeiras linhas dizem qual GPU e driver foram usados). |
| O editor de facepaint leva ~40 s na primeira vez | É esperado: ele prepara um cache no seu computador. As próximas aberturas levam segundos. |
| Sem som | Confira se o servidor de som funciona (`pactl info`); envie o log se funcionar. |
| Controles | Teclado e controles funcionam; ainda não há remapeamento. |

## Saves

Ficam em `local/user-nxrt`. O botão **Back up now** do launcher cria um zip em `local/backups`. Faça backup antes de atualizar.
