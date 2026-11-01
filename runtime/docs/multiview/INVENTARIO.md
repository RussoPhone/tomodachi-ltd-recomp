# Inventário de dependências do renderer para replay

Levantamento estático da branch `replay-t1`, base `f43a4c606c0a10164af45dc5b1e40ebec97cc12d`. Escopo: entradas lidas pelo renderer atual; não é uma proposta de serialização nem prova de replay funcional. As linhas apontam para `src/nvn/model.h`, `src/render/nvn_renderer.cpp` e `src/render/vk_context.cpp` nesta revisão.

## 1. Campos de `nvn::Model` lidos pelo renderer

| Campo | Tipo declarado em `model.h` | Uso no renderer |
|---|---|---|
| `pool_generation` | `u64` | `RefreshGpuPoolLookup`: invalida/recria o índice de faixas GPU quando a geração dos pools muda. |
| `pools` | `unordered_map<GuestAddr, MemoryPool>` | `RefreshGpuPoolLookup`, `GpuToCpuBacking` e `ImageKey`: traduz GPU→CPU e resolve o backing/identidade das imagens; também entra em estatísticas de memória. |
| `virtual_mappings` | `map<GpuAddr, VirtualMapping>` | `GpuToCpu` e `GpuToCpuBacking`: traduz faixas virtuais GPU para o pool físico e endereço CPU. |
| `textures` | `unordered_map<GuestAddr, Texture>` | `ImageKey`, `ImageFor`, `SampledImage`, inspeção de render targets e texturas de janela, e estatísticas/dumps em `Present`. |
| `programs` | `unordered_map<GuestAddr, Program>` | `GetPipeline`/`Draw`, `Dispatch`, `TraceDispatch` e traces de `ExecuteList`: resolve estágios, endereços de código/controle e serial do programa. |
| `texture_pools` | `unordered_map<GuestAddr, unordered_map<u32, pair<GuestAddr, GuestAddr>>>` | `SampledImage`/`Draw`: resolve índice de descriptor para textura e sampler; também conta entradas em estatísticas. |
| `texture_pool_views` | `unordered_map<GuestAddr, unordered_map<u32, TextureView>>` | `SampledImage`: recupera mip/layers da view registrada para a entrada do pool. |
| `windows` | `unordered_map<GuestAddr, Window>` | `Draw`, apenas no trace de draws de seis índices: identifica aliases entre render targets e texturas da janela. |
| `finished` | `unordered_map<u64, shared_ptr<CommandList>>` | `ExecuteList`, no caso `Op::CallCommands`: expande uma lista já finalizada chamada por outra lista. |
| `lock` | `std::mutex` | Adquirido em `Execute` e em trechos de `Present`/diagnóstico para sincronizar o acesso ao modelo. Não é dado serializável. |
| `texture_pool_registration` | `TexturePoolRegistrationStats` | `Present` com estatísticas de memória habilitadas; somente diagnóstico. |

Os demais membros de `Model` declarados em `model.h` não aparecem como `model_.campo` em `nvn_renderer.cpp`. Isso não afirma que o recorder não os usa: builders e `buffers`, por exemplo, participam da construção/decodificação das listas antes de `Renderer::Execute`.

## 2. Usos de `g_memory` no renderer

`src/render/nvn_renderer.cpp` é o único `.cpp` em `src/render/` com referências diretas a `g_memory`. A captura de páginas precisa cobrir acessos indiretos feitos durante `Execute` e `Present`, não apenas conteúdo apontado pelos pools do modelo.

| Função | Acessos / dados dependentes |
|---|---|
| `NvnEnvironment::ReadInstruction` e construtor | Lê instruções Maxwell e o cabeçalho `Shader::ProgramHeader` a partir do backing CPU do shader. |
| `NvnRenderer::RefreshGpuPoolLookup`, `GpuToCpuBacking`, `Format` | Lê GPU addresses nos objetos `MemoryPool`, tabela de formatos do jogo (`NXRT_NVN_FORMAT_TABLE`) e metadados de mapeamento/backing. |
| `ReadTic`, `ReadTsc`, `StorageBase`, `ReadCbuf`, `Translate` | Verifica/lê descritores TIC/TSC, tabela SSBO (`NXRT_NVN_SSBO_TABLE`), uniform buffers/const buffers e palavras de shader usadas na tradução/especialização. |
| `SampledImage` | Verifica endereços e mapeamento; lê/desfaz swizzle de pixels, mip levels e dados de texturas block-linear/3D/tiling. |
| `ReadPitch`, `RefreshPitch`, `RefreshSwizzled` | Lê backing de texturas pitch-linear e block-linear, compara shadow/hash e atualiza imagens amostradas. |
| `Draw` | Pode ler contagem de pools para trace; índices, vertex buffers, uniform/storage buffers, endereços de descritor e dados de probes/traces. Esses são inputs diretos do draw. |
| `ReportCounter` | Escreve resultados/counter timestamps na memória convidada e lê o controle de conditional rendering. |
| `GetComputePipeline`, `TraceDispatch` | Lê magic, dimensões de workgroup, limites de memória local/compartilhada e bytes do shader de compute para criar/logar o pipeline. |
| `Dispatch` | Lê UBO/SSBO e descritores; depois do dispatch pode copiar resultados de volta para a memória convidada. |
| `ExecuteList` | `Op::UpdateBuffer` escreve palavras inline; `BindTexturesDeferred` lê os handles pelo ponteiro CPU registrado; `RenderEnableConditional` lê os resultados de counters. |

### Requisito que emerge para a captura

A página associada a um evento `EXECUTE` deve representar o conteúdo de memória acessível naquele ponto, inclusive shader code/control, descritores, buffers e handles deferred que não são necessariamente enumeráveis só por `Model::pools`. `Dispatch` e `UpdateBuffer` também escrevem em memória: a ordem dos eventos e a atualização de páginas entre eventos precisam preservar essas escritas para a repetição.

## 3. Globais e variáveis de ambiente que influenciam desenho ou observação

Globais de processo que alimentam o renderer: `g_memory` (`GuestMemory`, `src/memory.cpp`); o singleton `nvn::GetModel()` e o renderer global escolhido por `nvn::SetRenderer()` (`src/nvn/recorder.cpp`); e o `SDL_Window*` passado a `CreateNvnRenderer`. O contexto Vulkan escolhe dispositivo/fila a partir das GPUs disponíveis e, se existir janela, da superfície apresentável.

As variáveis abaixo foram encontradas por `getenv` em `src/render/*.cpp` ou pelas chamadas dinâmicas `EnvOffset`. Variáveis de probe/trace/dump só observam o quadro, exceto quando indicado; não devem ser confundidas com o comportamento padrão.

| Grupo | Variáveis |
|---|---|
| Backend / estado que pode alterar o resultado | `NXRT_GPU_WAIT_MS`, `NXRT_LEGACY_INVALID_TEXTURE_SKIP`, `NXRT_PROBE_ARRAY_NULL_2D`, `NXRT_NO_FEEDBACK_SNAPSHOT`, `NXRT_NO_SAMPLED_REFRESH`, `NXRT_NO_TEXT_REUPLOAD`, `NXRT_NO_COMPUTE`, `NXRT_RENDER_CONDITIONAL`, `NXRT_NO_TARGET_LEVELS`, `NXRT_NO_NULL_TARGETS`, `NXRT_NO_STREAM_FLUSH`, `NXRT_NO_TARGET_LAYERS`, `NXRT_NO_NDC_CONVERT`, `NXRT_FORCE_NOCULL`, `NXRT_LEGACY_VIEWPORT`, `NXRT_KEEP_FRONT_FACE`, `NXRT_YFLIP_INVERT_FRONT`, `NXRT_PROBE_EQUAL_AS_LEQUAL`, `NXRT_NO_TEX_MIPS`, `NXRT_COMPUTE_WG`, `NXRT_PROBE_FLOOR_D32_Y_MIRROR`, `NXRT_PROBE_SHADOW_ALWAYS`, `NXRT_PROBE_SKIP_WATER_DRAW`, `NXRT_SWAPCHAIN_SRGB`, `NXRT_NVN_FORMAT_TABLE`, `NXRT_NVN_SSBO_TABLE`. |
| Seleção de trace/censo (instrumentação) | `NXRT_TRACE_PRESENT_CORRELATION_FRAME`, `NXRT_TRACE_PRESENT_CORRELATION_RADIUS`, `NXRT_TRACE_WIN_SAMPLE`, `NXRT_TRACE_WIN_SAMPLE_FRAME`, `NXRT_TRACE_WIN_SAMPLE_SERIAL`, `NXRT_TRACE_WIN_SAMPLE_PROGRAM`, `NXRT_TRACE_WIN_SAMPLE_INDEX_COUNT`, `NXRT_TRACE_WIN_SAMPLE_TARGET_WIDTH`, `NXRT_TRACE_WIN_SAMPLE_TARGET_HEIGHT`, `NXRT_TRACE_WIN_CHECKPOINT_DRAWS`, `NXRT_PROBE_DRAW_CENSUS`, `NXRT_PROBE_CENSUS_DETAIL_PROGRAM`, `NXRT_PROBE_CENSUS_CHECKPOINTS`, `NXRT_PROBE_CENSUS_CHECKPOINT_GEOMETRY_COUNTS`, `NXRT_PROBE_CENSUS_CHECKPOINT_TARGET`, `NXRT_PROBE_CENSUS_CHECKPOINT_BEFORE_TARGET`, `NXRT_PROBE_CENSUS_CHECKPOINT_BEFORE_GEOMETRY_COUNTS`, `NXRT_PROBE_CENSUS_SAMPLE_TARGET`, `NXRT_PROBE_CENSUS_SAMPLE_PIXELS`, `NXRT_PROBE_CENSUS_UBO_HASH_RANGE`, `NXRT_PROBE_CENSUS_GEOMETRY_SEQS`, `NXRT_PROBE_CENSUS_GEOMETRY_COUNTS`, `NXRT_PROBE_CENSUS_UBO_PREFIX_SLOTS`, `NXRT_PROBE_CENSUS_UBO_PREFIX_BYTES`, `NXRT_TRACE_SHADER_FAILURE`, `NXRT_TRACE_ASTC`, `NXRT_TRACE_SAMPLED_REFRESH`, `NXRT_TRACE_GLYPH`, `NXRT_TRACE_DRAWS`, `NXRT_TRACE_CMD`, `NXRT_TRACE_COUNTERS`, `NXRT_TRACE_COMPUTE`, `NXRT_TRACE_RAW_PIXEL`, `NXRT_TRACE_RAW_PIXEL_X`, `NXRT_TRACE_RAW_PIXEL_Y`. |
| Dumps/saída de diagnóstico | `NXRT_DUMP_SHADER_PROGRAM`, `NXRT_DUMP_MODEL_TEXTURE_DIR`, `NXRT_DUMP_COMPUTE_SPIRV`, `NXRT_DUMP_COMPUTE_PROGRAM`, `NXRT_DUMP_DEPTH_SAMPLES`, `NXRT_DUMP_DIR`, `NXRT_DUMP_EVERY`, `NXRT_DUMP_LAYER`, `NXRT_DUMP_SELECTED_TARGET`, `NXRT_DUMP_TARGETS`, `NXRT_DUMP_TARGET_KEY`, `NXRT_DUMP_TARGET_FRAME`, `NXRT_NVN_STATS`, `NXRT_SCREENSHOT`, `NXRT_SCREENSHOT_FRAME`. |
| Estatística / timing | `NXRT_RENDER_STATS`, `NXRT_FRAME_PROFILE`, `NXRT_TRACE_RENDER_MEMORY`, `NXRT_TRACE_IMAGE_RECREATE`, `NXRT_PROBE_FRAME_SLEEP_MS`, `NXRT_TRACE_GLYPH_DUMP`. |

`NXRT_TRACE_GLYPH` pode provocar reupload de uma textura de glifo pitch-linear para diagnóstico; `NXRT_PROBE_*` cujo nome indica skip/mirror/equal/shadow altera deliberadamente o caminho observado. Para comparação fiel, essas opções precisam estar ausentes, salvo quando o teste pede explicitamente um A/B.

## Respostas pedidas

- **O renderer acessa `recording`/`finished`?** Não acessa `model_.recording`. Acessa `model_.finished` em `ExecuteList` para expandir `Op::CallCommands`; a lista chamada pode conter outras chamadas aninhadas, limitadas pelo argumento `depth` do executor. Portanto a captura precisa preservar os `CommandList` finalizados alcançáveis por `CallCommands`, além da lista externa entregue ao `Execute`.
- **Quais structs de `model.h` precisam ser serializadas?** Para reconstruir o estado que o renderer efetivamente consulta: `MemoryPool`, `VirtualMapping`, `TextureDesc`/`Texture`, `TextureView`, `Program`/`ShaderStage`, `Window` e os mapas de pool de textura/views com seus pares de `GuestAddr`; para comandos: `Command` e `CommandList`. `Model` precisa ser reconstruído como estado lógico, sem serializar diretamente `std::mutex lock`. `TexturePoolRegistrationStats` e `FrameStats` são estatísticas e não são necessárias para o desenho. A S2 deve decidir explicitamente se serializa apenas o subconjunto usado ou todas as entradas dos campos usados; builders não lidos diretamente pelo renderer não são requisito demonstrado por este inventário.

- **`CreateNvnRenderer(nullptr)` funciona sem janela?** Pelo código, `VulkanContext::Init(nullptr)` não cria superfície, swapchain ou recursos de apresentação e ainda cria o dispositivo/fila Vulkan. `NvnRenderer::Init` segue com os recursos offscreen; `PresentImage` retorna sem swapchain. Assim, a construção sem janela é suportada para renderização offscreen/dumps se a inicialização Vulkan funcionar no host. Isso é conclusão de inspeção estática, não um teste de runtime nesta fatia.

### Decisão aplicada na T1-S2 sobre `Model::lock`

`lock` é uma primitiva de sincronização do processo, não estado do jogo: `WriteModel` adquire o mutex enquanto copia os campos lógicos usados pelo renderer e não grava o mutex. `ReadModel` constrói um `Model` novo, cujo mutex nasce desbloqueado. O renderer só precisa desse mutex para proteger o acesso concorrente ao estado reconstruído. Essa decisão resolve a impossibilidade de copiar/serializar `std::mutex`; o restante dos campos diretos listados acima é mantido no snapshot. `recording`, builders, buffers, estatísticas de frame e contadores não lidos diretamente pelo renderer ficam fora do snapshot da S2.

## Complemento identificado na execução da S4 (2026-10-18)

O inventário inicial omitiu `nvn::DriverModuleBase()`, global de `nvn_layer.cpp`. `Format()` consulta `base + NXRT_NVN_FORMAT_TABLE` (padrão `0xb69208`); `StorageBase()` consulta `base + NXRT_NVN_SSBO_TABLE` (padrão `0x915550`). O replay precisa restaurar esse endereço e capturar as páginas do ponteiro/tabelas, além dos pools. A API de restauração adicionada só é chamada pelo executável de replay; nenhum driver guest é executado.

`ReadTic` e `ReadTsc` também leem os cabeçalhos dos objetos de pools de descritores (20 bytes) e o endereço CPU em `NVNmemoryPool + 0x30`. Esses objetos podem ficar fora do storage dos pools. A captura passou a guardar essas páginas explicitamente. Por fim, `finished` é mutável entre envios: o snapshot inicial sozinho não contém listas finalizadas depois do início da gravação. Eventos STATE guardam os recursos e a árvore transitiva de listas de cada EXECUTE, preservando o limite de profundidade 4 do renderer.
