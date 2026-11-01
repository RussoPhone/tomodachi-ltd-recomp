# T1-S4 — reprodutor offscreen

Verificação em 2026-10-18, worktree `nxrt-replay`, branch `replay-t1`, implementação iniciada sobre `f5fe6fd`.

## Resultado

**S4 pronta como ferramenta de execução.** O build `build-replay -j3` passou; CTest26/26 e `git diff --check` passaram. A revisão de código terminou sem Critical/Important pendente. Renderer e recorder não foram editados. A geração de imagens não aprova fidelidade: essa é a condição separada da S5.

Captura privada canônica: `orq/logs/N1/T1R-captura-q11000-driver4/captura`, relativa à raiz SWITCHPILER. A rodada terminou com `rc=130`, `save=identical`, `capture=ok`, dentro do timeout1180s; log de conclusão em1134.465s. São25eventos,109447páginas únicas,427.53MiB em páginas e776MiB de captura total. Referências q11000 em seu subdiretório `dump`.

Auditoria privada atravessou as listas efetivamente usadas: nenhuma página ausente em arrays deferred, controles compute ou objetos/backings de pools de descritores. Isso também confirma a validade desta captura apesar de uma correção genérica posterior que ampliou a descoberta de pools em listas aninhadas: os pools usados neste caso já estavam cobertos. A captura preserva o hash do binário que a produziu; o replay foi recompilado com a implementação final antes de sua execução.

Replay em Intel(R) Graphics(RPL-S), timeout60s, exit0:25eventos em **5.381s**, gerando `frame-11000-selected-target.ppm` e `frame-11000-selected-present.ppm`. Evidência privada em `orq/logs/N1/T1R-replay-q11000-driver4-a/`.

## Implementação

`nxrt-replay <pasta>` carrega o Model, valida índice/SHA-256 das páginas, mapeia memória guest e reaplica STATE/EXECUTE/PRESENT em Vulkan sem janela. Não executa jogo ou driver guest. Stubs exclusivos do executável satisfazem referências de runtime presentes no arquivo estático nxrt_core.

O gravador inclui:

- Pré-roll no Present anterior ao primeiro quadro, para incluir o Execute que o desenha.
- Snapshot por Execute dos recursos e da closure transitiva de listas, com profundidade4 igual à do renderer.
- Páginas dos objetos de pools de memória (`+0x18`, `+0x20`, `+0x30`), headers de pools de descritores, inclusive os usados em listas aninhadas sem registro no Model.
- `DriverModuleBase`, tabelas de formatos e SSBO; API de restauração usada somente pelo replay.
- Arrays CPU de `BindTexturesDeferred` e seções `control` dos programas, mesmo fora do storage dos pools.
- Snapshot STATE com limite simétrico64MiB em escrita e leitura.

O replay prepara o contador interno por Present(0,n), sem inventar conteúdos anteriores de imagens. Sem variáveis de captura, o caminho normal continua usando o renderer original.

## Limites e histórico

A revisão registrou um Minor: ReportCounter produz contadores/timestamps durante Execute, usando estado acumulado e relógio do renderer. A captura não reconstrói todo o histórico desses valores. Conteúdos anteriores de render targets, caches e bindings herdados também precisam da validação S5; não foram considerados automaticamente resolvidos.

Capturas anteriores não são evidência de fidelidade: `preroll1` completou depois da perda do runner, mas faltavam dependências. `driver3` produziu imagens em4.530s/4.369s bit a bit determinísticas, porém a auditoria encontrou606 ocorrências de arrays deferred e12 de controles compute sem páginas. Sua diferença visual de59.89%HDR/58.77%present foi classificada como entrada incompleta e motivou retorno ao gravador, não aprovação S5. Saves, páginas, imagens e logs permanecem fora do Git; sem push.

## Resultado posterior da S5

A ferramenta da S4 foi commitada em94ce22d. A S5, com a captura auditada driver4, ficou BLOQUEADA:41.66%HDR e44.33%present de diferença contra o original;71/47pixels diferem entre replays sem tolerância. Veja `VALIDACAO.md`. Critério operacional de geração das imagens não implica fidelidade; S6/S7 não foram iniciadas por determinação explícita do plano acima de5%.
