# Roteiro: o nosso próprio recompilador

🇬🇧 [English](ROADMAP-RECOMPILER.md)

Isto é uma **direção, não um cronograma**. A prioridade número um continua sendo estabilizar o que existe (instalador, runtime, desempenho).
O trabalho no recompilador é um processo contínuo, que move uma peça por vez e sempre com o jogo funcionando.

## Por que

Hoje o instalador usa o [mk8-recomp](https://github.com/dougchansan/mk8-recomp) (baseado em código do suyu) para transformar o código ARM do
jogo em C. Essa é a parte mais lenta e pesada da instalação, medida numa instalação limpa (notebook de 16 threads, 16 GB de RAM):

| | Medido |
|---|---|
| C gerado | ~4,5 GB em ~800 unidades de compilação |
| Compilar | 99 min de relógio, ~780 min de CPU no total |
| Onde vai o tempo | as 30 maiores unidades são 55% do tempo de CPU; uma unidade de 45 MB leva ~6 min sozinha e ~3,6 GB de RAM |
| Instalação completa | cerca de 2 h 10 min e ~27 GB de disco com a primeira configuração (15000 instruções por unidade). Dividir em unidades menores (6000) reduziu o passo de compilação para 78 min e o pico de RAM por compilação de 3,5 GB para 1,6 GB, com o mesmo total de C, e agora é o padrão |

Parte desse tamanho vem do jeito de gerar o código: cada bloco leva uma guarda que confere de novo as palavras da instrução (para poder cair
num JIT) e um seletor de entrada. O runtime (`nxrt`) não tem JIT, então um recompilador nosso não precisa de nenhum dos dois e pode gerar bem
menos C. Trocar o recompilador também tiraria o suyu das ferramentas do instalador.

## Como as peças se encaixam hoje

- O código gerado é C puro. Cada bloco do jogo é `void blk_<módulo>_<endereço>(GuestContext*)`, e cada módulo exporta
  `recomp_image_lookup_<módulo>(endereço)`, que devolve a função daquele endereço.
- O contrato entre esse código e o runtime é pequeno: `GuestContext` (registradores, flags, vetores, `pc`, um número de SVC pendente) e
  `RecompHostMem` (funções de memória e de acesso exclusivo). Ele fica em [`runtime/src/guest_abi.h`](../runtime/src/guest_abi.h) e
  [`runtime/src/cpu.cpp`](../runtime/src/cpu.cpp).

Como o runtime só pergunta "quem executa este endereço?", **dois recompiladores podem conviver**: o nosso responde pelos endereços que cobre
e o atual responde pelo resto.

## Fases

| Fase | O quê | Como sabemos que está certo |
|---|---|---|
| 0. Estabilizar (agora) | Testes de instalação limpa, issues, 1.7.x | Relatórios do `./install.sh --report` |
| 1. Extração própria | Ler o `.nsp`, descriptografar e extrair os módulos sem o suyu | Os hashes dos módulos batem com os de `adapters/tomodachi/target.json` |
| 2. Recompilador v0 | Mesma assinatura de bloco e mesmo `GuestContext`; começar pelo módulo pequeno `rtld` e por funções simples; sem guardas de JIT | Teste diferencial contra um emulador de CPU de referência, mais as rotas de replay da ilha |
| 3. Cobertura crescente | O módulo `sdk`, depois o `main`, função por função | Um relatório de "% de funções vindas do nosso recompilador" |
| 4. 100% nosso | O mk8-recomp sai do instalador; o contrato pode mudar à vontade | O jogo completo roda e o instalador não baixa mais o suyu |

## Regras da transição

- **Sempre um jogo funcionando.** Cada passo é conferido com o jogo e o replay antes de seguir.
- **Bisseção por endereço.** Uma variável de ambiente escolhe qual recompilador responde por uma faixa de endereços, então uma função errada
  se acha por bisseção, não por palpite.
- **Primeiro cumprir o contrato,** e só mudá-lo depois que o recompilador antigo sair.
- **Medir antes e depois:** tamanho gerado, tempo de compilação e pico de RAM, e o tempo de frame das rotas de replay.

## Riscos

- Instruções vetoriais, exatidão de ponto flutuante e exceções são onde recompiladores erram; o teste diferencial precisa cobri-los bem.
- O contrato atual tem pontos finos (geração de guardas, o formato da memória rápida) que precisam ser reproduzidos exatamente no começo.
- A velocidade do código recém-gerado dentro do jogo é desconhecida até ser medida.

## Coisas baratas para tentar primeiro (ainda não feitas)

- `export-aot.py --backend static` em vez de `hybrid`. O backend híbrido é o que inclui as guardas de JIT; se o runtime aceita a saída
  estática e quanto ela diminui são **hipóteses não testadas**.
- Unidades ainda menores (`unit_insns` abaixo de 6000): em teste.

## Créditos e licenças

Estudamos outros recompiladores para aprender com eles. Qualquer código de outro projeto só é usado sob a licença dele e com crédito. Os
projetos citados neste repositório são GPL-3.0, como este.
