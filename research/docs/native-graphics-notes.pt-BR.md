# Missão 5 — 100% nativo

Pedido do usuário (2026-10-08): continuar até o Tomodachi Life rodar 100% nativo, com commit a cada marco.

## O que ainda não é nativo

| Camada | Hoje | Natureza |
|---|---|---|
| CPU | código do jogo traduzido para x86-64 (AOT), 0 JIT | nativo |
| Serviços do sistema (HLE) | reimplementação em C++ do host (arquivos, saves, contas, controles) | nativo (camada de compatibilidade, como o Wine) |
| Áudio | `audio_core` reimplementa o renderizador de áudio em C++ do host → SDL3 | nativo (não emula o DSP) |
| **GPU** | o driver NVN do jogo (código traduzido) gera comandos Maxwell; o `video_core` interpreta esses comandos e os converte para Vulkan | **emulação do processador de comandos da GPU** |

O que falta é a GPU: responder às chamadas NVN do jogo diretamente com Vulkan, sem comandos Maxwell no meio.

## Plano

1. **Rastreio NVN (patch 0009)**: ganchos no despachante interceptam `nvnBootstrapLoader` e `nvnDeviceGetProcAddress`;
   cada função NVN pedida pelo jogo recebe um endereço falso que volta ao host, é contada e segue para o driver real.
   Resultado: a lista exata de funções NVN usadas, quantas vezes, com argumentos, e o endereço de cada uma no driver
   (nomes para o `sdk`).
2. **Modelo de objetos NVN no host**: Device, Queue, MemoryPool, Buffer, Texture, Sampler, CommandBuffer, Program,
   Window, Sync. Layouts deduzidos do rastreio e da decomp do driver.
3. **NVN → Vulkan**: memória (pools espelhados), texturas (decodificadores do suyu), shaders Maxwell → SPIR-V na instalação
   (shader_recompiler do suyu), comandos de desenho, apresentação.
4. Modo `SUYU_NVN_HLE=1` até a primeira tela; depois ampliar até o jogo inteiro; por fim tornar padrão.

## Marco 1 — rastreio NVN (2026-10-08)

- Patch 0009 (`src/core/arm/recomp/nvn_trace.{h,cpp}` + gancho no despachante): funciona sem alterar o jogo.
  Os módulos se registram como `nnrtld`, `Colony.nss` (main) e `nnSdk`: `SUYU_NVN_BOOTSTRAP=nnSdk:0x502d10`.
- 90 s na tela inicial: o jogo pede **534** funções NVN e chama **206**. Por objeto: CommandBuffer 46, TextureBuilder 21,
  MemoryPool 13, Texture (get) 10, SamplerBuilder 10, QueueBuilder 8, Sampler (get) 8, TextureView 7, DepthStencil 7,
  WindowBuilder 6, estados (polígono, blend, multisample, cor, máscara de canal), vertex attrib/stream, programas, pools.
- Volume: milhões de chamadas de estado (ChannelMaskState 2,2 M, BindTexture 720 k, Blend* 560 k): o jogo reconstrói os
  objetos de estado a cada desenho. A implementação no host precisa ser barata nesses setters (são só gravações em structs).
- Os 534 endereços viram nomes (fato observado) para o `sdk.elf`: `names/sdk-nvn.jsonl` → 22 576 símbolos no sdk.
- Rastreio salvo em `local/analysis/tomodachi/nvn/nvn-trace-title.tsv` (com os registradores das 3 primeiras chamadas).

## Marco 2 — modelo sombra do NVN (2026-10-08)

- Patch 0010: interface de observador nos ganchos (antes da chamada e, quando pedido, no retorno) e a biblioteca
  `src/nvn_hle` (`SUYU_NVN_HLE=shadow`), que decodifica as chamadas num modelo do host: pools, buffers, texturas,
  samplers, programas, listas de comandos gravadas (por handle de `EndRecording`). Estados pequenos (blend, cor, máscara,
  profundidade/stencil, polígono, multisample, atributos/streams de vértice) são lidos com o layout do driver:
  - blend (8 B): w0 bits 0-2 alvo, 24-27 equação cor, 28-31 equação alfa; w1 = src/dst cor, src/dst alfa (1 byte cada)
  - cor: bits 8-15 blend por alvo, 16-23 operação lógica; máscara de canal: 4 bits (RGBA) por alvo
  - profundidade/stencil (8 B): w0 bit0 teste, bit1 escrita, bit2 stencil, bits 4-7 função; w1 frente: função 0-3,
    falha 4-7, passa 8-11, falha-de-profundidade 12-15; trás nos bits 16-31
  - polígono: bits 0-1 cull, 2 frente, 3-4 modo, 15-18 offsets; atributo de vértice (4 B): stream 0-4, ativo 6,
    offset 7-20, formato 21-31; stream de vértice (8 B): stride, divisor
  - enums no estilo OpenGL (depth LESS=2, stencil ALWAYS=8, blend ZERO=1/ONE=2); topologia e tipo de índice são os do
    Maxwell (o driver grava direto). Handle de textura = id | sampler << 20 | 1 << 32.
- Tela inicial: ~21 desenhos/frame, ~2 400 comandos/frame; 1 837 pools, 28 375 buffers, 4 869 texturas, 1 172 samplers,
  6 597 programas. Alvos de renderização de 8×8 a 1920×1080; formatos amostrados f1, f10, f37, f41, f46, f63, f73, f75.
  Todas as texturas amostradas resolvidas; ~2 handles de lista desconhecidos por frame (a investigar).
- O gravador custa FPS (31 → ~21) por causa de std::function + mutex por chamada; otimizar antes de virar padrão.
- `scripts/mkpatch.sh` gera um patch contra o suyu limpo + patches anteriores; `scripts/verify-patches.sh` confere que a
  pilha inteira reproduz a árvore.

## Marco 3 — renderizador sombra NVN → Vulkan (2026-10-08)

- Patch 0011. Primeira imagem desenhada pelo nosso renderizador (Vulkan próprio, sem a GPU emulada): a tela de
  interface do jogo (fundo, botões, cursor) no alvo de cena 1920×1080 R11G11B10F. Dezenas de milhares de desenhos sem
  pular nenhum na tela inicial.
- Descobertas: dados de shader NVN = cabeçalho de 0x30 bytes (mágica 0x12345678, tamanho, ponteiro de controle) + SPH;
  a base do programa é o início dos dados (alinhamento das palavras de escalonamento a cada 32 bytes); estágios NVN
  vértice 0, fragmento 1, geometria 2, tess. controle 3, tess. avaliação 4, compute 5; c2 = buffer do driver (handles de
  textura em 0x20 + 8i, storage buffers em slots de 16 bytes a partir de tabela sdk+0x915550); uniforms em slot i + 3;
  palavra TIC da tabela de formatos em +0x24 (+0x20 é quase igual, deslocada 1 bit); pool: GPU em +0x18 e +0x20;
  MapVirtual: requisições de 40 bytes (pool físico, offset físico, offset virtual, tamanho, classe); a janela tem 2
  texturas (buffer duplo) sobre o mesmo pool; imagens do host indexadas pela memória (pool+offset).
- Pendências: texto (provável ASTC, sem suporte na GPU desktop: decodificar com o decodificador do suyu), composição na
  textura da janela (o buffer apresentado ainda não recebe o desenho), texturas array/3D, mipmaps, conferir orientação.
- Armadilha: o logger do suyu morre se recebe milhares de mensagens por segundo; diagnósticos via stderr.

### Investigação: composição na janela (em andamento)

- A janela tem 2 texturas (índices alternam a cada present). Nenhuma recebe desenho do renderizador sombra.
- Lista que liga a textura da janela: SetRenderTargets, BindProgram, BindTexture, BindVertexAttrib/Stream e acaba.
  As listas seguintes trazem SetSamplerPool/SetTexturePool, CallCommands e ~130 BindTexture, sem desenho.
- O modelo tem poucas listas finalizadas (handles reaproveitados a cada frame, anel de memória de controle). Há
  command buffers "órfãos": Initialize/AddMemory/SetMemoryCallback, BindTexture ×40 e EndRecording sem
  BeginRecording observado; o handle que eles devolvem é chamado depois por CallCommands e não está em `finished`
  (contagem: EndRecording chamadas = retornos, 0 sem par; alguns "sem lista").
- Próximo: registrar a ordem completa das chamadas de um command buffer órfão desde o Initialize (antes do 1º
  EndRecording), conferir se ListFor/Record usam a mesma chave, e se o desenho final da janela aparece em alguma
  lista; hipóteses: comandos gravados com outro ponteiro (cópia do objeto), ou desenho emitido por callback de memória.
- Atualização (mesmo dia): (1) handles de lista agora calculados na ENTRADA do EndRecording: chunk de controle em
  cmd+0x48, handle = chunk+0x10 ou (chunk+0x18)|count<<48|1 (NormalizeHandle unifica); sem trampolim de retorno.
  (2) Ganchos também nos endereços reais do driver (chamadas indiretas feitas de dentro do sdk, ex. via PLT do
  próprio sdk); só OnCall ali (trampolim de retorno nesses pontos quebrava: retorno em 0xfffffffe00000030).
  (3) Endereço de GPU dos pools lido da struct (+0x18/+0x20) sob demanda. Resultado: 0 handles desconhecidos,
  ~1,62 M chamadas/5 s observadas, mas AINDA nenhum desenho com alvo = textura da janela.
  (4) O sdk não chama nvnCommandBufferDraw*/SetRenderTargets diretamente (0 chamadores internos).
  Hipótese atual: o desenho de composição está em listas "vazias" (Initialize → EndRecording sem chamadas vistas,
  depois usadas por CallCommands): comandos escritos sem a API (cópia de comandos pré-gravados / escrita direta
  na memória de comandos) ou por entradas NVN não pedidas por GetProcAddress. Próximo: comparar a memória de
  comandos dessas listas (AddCommandMemory → região) antes/depois; ver se o jogo grava métodos Maxwell nelas.
- Atualização 2: listas reaproveitadas: uma gravação vazia termina no mesmo chunk e gerava o mesmo handle,
  sobrescrevendo a lista boa; agora a lista com comandos é mantida (apareceu o 1º desenho na janela: DrawElements,
  987 índices). DrawArrays/DrawElements simples agora gravados; DrawTexture/indiretos/compute só registrados (não
  chamados na tela inicial). As listas "vazias" chamadas na composição são vazias também no driver (controle:
  marcador de início, 0, 2 = fim). Mesmo assim quase nenhum frame tem desenho com alvo = textura da janela.
  Sequência por frame: SetRenderTargets(janela), BindProgram, BindTexture, atributos; listas seguintes: pools,
  CallCommands (130 BindTexture + 14 uniforms), 2 chamadas vazias, estados (blend/cor/profundidade/viewport) e fim.
  Próximo passo recomendado: na GPU emulada do suyu (ainda roda ao lado), registrar o draw do Maxwell3D cujo alvo
  de cor tem o endereço GPU da textura da janela e o endereço GPU/lista de comandos de onde veio; mapear de volta
  para a lista NVN (handle = chunk de controle). Isso mostra o que o gravador está perdendo.
- Atualização 3 (gabarito pela GPU emulada): SUYU_DEBUG_RT_VA=<va> (patch 0014, draw_manager.cpp) mostra os draws do
  Maxwell3D emulado num alvo. Janela (textura apresentada) = GPU 0x501d80000 (mapeamento de textura; pitch 0x500ca0000),
  estável entre execuções. A composição é um draw indexado de 6 índices (quad), topologia 4, programa VS em
  0x400026000 e FS em 0x400026400 (dados NVN; SPH em +0x30), todo frame.
- No nosso lado: o programa da composição é ligado no command buffer principal (o que tem BeginRecording e
  CallCommands); os draws de 6 índices são gravados em OUTROS command buffers uma vez (frames 0-1) e reaproveitados
  via CallCommands. Na sequência da janela, o CallCommands resolve para uma lista de 142 comandos só com binds: a lista
  verdadeira se perde por colisão/forma de handle.
- Handles: lista vazia → chunk+0x10 (não marcado); lista com comandos → em geral (chunk+0x18)|count<<48|1 (marcado),
  mas a lista principal volta não marcada. O chunk lido em cmd+0x48 na ENTRADA do EndRecording nem sempre é o que o
  driver usa (handle marcado 0x100214b28b1d9 continua desconhecido). Estado atual (patch 0014): listas com comandos
  registradas nas duas formas, vazias só na não marcada sem substituir listas com comandos; NormalizeHandle só tira
  os bits de contagem. PRÓXIMO: obter o handle exato pelo retorno de EndRecording quando a chamada vem do jogo
  (trampolim funciona nesse caminho; só nas chamadas internas do sdk ele quebrava) e usar o cálculo na entrada
  apenas como reserva; ou ler param_1[9] DEPOIS da chamada (OnReturn) em vez de antes.
- Atualização 4: mapeamento pelo valor de retorno do EndRecording (quando o jogo chama; patch 0015) também não
  fez a composição aparecer (texturas da janela continuam com 0 desenhos). Falta descobrir por que a lista com o
  draw de 6 índices não é executada no CallCommands da composição: comparar o handle passado ao CallCommands com
  os handles registrados (retorno e entrada) para o command buffer onde o draw foi gravado. Pausado a pedido do
  usuário em 2026-10-08.
