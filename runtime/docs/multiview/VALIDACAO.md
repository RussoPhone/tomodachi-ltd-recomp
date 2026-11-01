# T1-S5 — validação de fidelidade

Data: 2026-10-18. **BLOQUEADA. S6 e S7 não foram iniciadas.**

A ferramenta executa o replay rapidamente e passa o critério operacional da S4, mas ainda não reproduz fielmente a imagem original. Não usar estas capturas como referência confiável de regressão gráfica até resolver a diferença abaixo. Não há declaração de piso corrigido ou de port concluído.

## Versão e execução

- Commit da ferramenta: `94ce22d` (`T1-S4: reprodutor offscreen e dependencias de captura`), branch `replay-t1`, build `build-replay`, Clang/Ninja, `-j3`.
- Build final e CTest26/26 passaram; diff-check limpo; revisão sem Critical/Important pendente. Renderer e recorder não foram alterados.
- Captura privada canônica: `SWITCHPILER/orq/logs/N1/T1R-captura-q11000-driver4/captura`.
- Quadros10998:3, com pré-roll em10997;25eventos;109447páginas únicas;427.53MiB de páginas;776MiB na pasta de captura. SHA-256 do capturador no manifest e em `binary.sha256` da rodada.
- Intel(R) Graphics(RPL-S)/Vulkan, replay80 e cópia descartável de k235-save. `run-q11000-ilha.sh` original sob flock; timeout1180s. Conclusão em1134.465s, `rc=130`, `save=identical`, `capture=ok`.
- Auditoria privada da closure dos eventos: nenhuma página faltante nos arrays deferred, controles compute, headers de descritores ou ponteiros de seus backings usados. A correção genérica de pools aninhados feita após o início da captura não altera a validade deste conjunto: todos os objetos efetivamente usados já estavam cobertos.
- Replays A e B, ambos com timeout60s, exit0: **5.381s** e **4.478s**. Ambos produziram as duas imagens q11000.

## Repetição versus original

Comparador existente `TOOLS/comparar-regioes/comparar_regioes.py`, tolerância8 por canal. Dimensões1920×1080. Regiões: tela=(0,0,1920,1080), água=(0,0,1920,400), ilha=(300,300,1200,500).

| Imagem | Tela: pixels diferentes | Água | Ilha | Diferença média normalizada, tela |
|---|---:|---:|---:|---:|
| selected-target (HDR convertido para PPM) | **41,66%** | 61,32% | 24,38% | 0,031 |
| selected-present | **44,33%** | 69,80% | 23,47% | 0,034 |

O limite do plano é0,5% na tela inteira. Ambos falham. Os JSONs completos estão no diretório privado do replay A.

## Repetição versus repetição

Comparação exata dos pixels, sem arredondamento, e arquivos comparados com cmp/SHA-256:

| Imagem | Pixels diferentes | Total | Porcentagem exata | Maior diferença por canal | Pixels que diferem mais de8 |
|---|---:|---:|---:|---:|---:|
| HDR | 71 | 2073600 | 0,0034239969% | 6 | 0 |
| Apresentação | 47 | 2073600 | 0,0022665895% | 3 | 0 |

Os JSONs arredondados do comparador exibem0,00% na tela inteira, mas isso não é igualdade exata. Os arquivos diferem e falham o requisito de determinismo0%. Esses desvios são pequenos; não explicam a diferença grande entre replay e original.

## Evidência visual e interpretação

Comparação lado a lado, fora do Git: `SWITCHPILER/orq/logs/N1/envio/T1R-validacao.png`. Original à esquerda e replay A à direita; HDR na linha superior e apresentação na inferior.

Fatos observados:

- Geometria, posições dos edifícios e desenho geral da ilha aparecem em ambas as imagens.
- O evento completo de geometria do draw selecionado é idêntico nos logs original/replay:1350índices,256vértices únicos e os mesmos atributos/bytes registrados.
- A água perde parte do detalhe e da cor costeira no replay; elementos de texto/decoração da interface também diferem. Isso é compatível com as regiões quantitativas acima, mas não demonstra uma causa única.
- As dependências CPU conhecidas estão presentes. A auditoria é cobertura de endereços usados; não é prova de que todo estado do renderer foi reconstruído.

Hipóteses que continuam sem comprovação causal: conteúdos anteriores de imagens/render targets, bindings/caches herdados e estado de contadores do renderer. A revisão registrou que ReportCounter escreve memória usando relógio real e acumuladores durante Execute; esses valores não são restaurados integralmente pelo formato atual. Não foi demonstrado que essa diferença explica os pixels observados. Também não foi provado que os71/47pixels entre replays vêm de ReportCounter ou de compute.

## Por que a execução parou na S5

O plano `orq/PLANO-T1-REPLAY.md`, linha113, permite uma captura10990:11 somente quando a diferença fica entre0,5% e5%. **Acima de5% manda registrar BLOQUEADO e produzir o lado a lado.** Esse limite foi atingido na entrada auditada. Não foi feita a tentativa de11quadros nem iniciadas S6/S7.

Próxima decisão técnica: revisar a estratégia de captura do estado inicial do renderer, distinguindo conteúdo GPU pré-existente, estado de bindings e contadores. O formato atual guarda objetos NVN, memória CPU e callbacks; não exporta/importa todo o estado interno/GPU do renderer. Essa extensão precisa de uma revisão do plano, cujas regras atuais mantêm o renderer inalterado. Depois, repetir S4/S5 antes do catálogo de três pontos, do comando único e da documentação de conclusão.

## Evidências privadas preservadas

- Captura original/exit/save hashes: `orq/logs/N1/T1R-captura-q11000-driver4/`.
- Replay A, métricas original×replay e auditoria de CPU: `orq/logs/N1/T1R-replay-q11000-driver4-a/`.
- Replay B, métricas determinísticas exatas e SHA-256: `orq/logs/N1/T1R-replay-q11000-driver4-b/`.
- Comparação visual: `orq/logs/N1/envio/T1R-validacao.png`.

Esses diretórios contêm memória/imagens do jogo; não foram commitados nem publicados. A captura driver3 anterior tinha entradas CPU comprovadamente ausentes e seus números não foram usados no aceite desta etapa.
