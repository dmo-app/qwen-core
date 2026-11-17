# Estado do Core

Actualizado em 17-11-2026. Base desta organização: `5445152e522ed071038c267a3f98bed48e370b44`.
Este ficheiro é o ponto de passagem entre auditores; regras em [AGENTS.md](AGENTS.md), procedimento em [docs/AUDITORIA.md](docs/AUDITORIA.md).

## Feito e evidência

| Entrega | Estado | Fonte |
| --- | --- | --- |
| Audit de bugs de 17/11 | Reporta 21 itens corrigidos e 10 abertos; 15 testes, 113 casos, 1456 verificações em Linux | [Audit](docs/relatorios/2026-11-17-auditoria-core.md); não reexecutado nesta organização |
| Produto e fronteira | Documentados; commits ecd2c7a e 5445152; não implementam limpeza | [Produto](docs/DIRECCAO-CORE-PATTERN-MAKER-TOOLS.md), [fronteira](docs/FRONTEIRA-CORE-AICORE.md) |
| Catálogo | Inventário de 45 patterns: 39 origem AICORE, 6 outras; preservação decidida | Anexo da fronteira / PROD-03 |
| Organização documental | Regras, índice, procedimento, modelo e relatório; sem alteração de código | [Relatório](docs/relatorios/2026-11-17-organizacao-repo.md) |
| Criação de música fora do core-main → `dmo-app/core-create` | Extracção só de leitura de `origin/main` = `52d46c0`; 8 estilos, 45 páginas de padrão (1034 notas conferidas contra o git), 21 fichas de 3,2 MB → 68 KB. **Só markdown, 0 JSON**, por instrução do dono de hoje; core-main intacto | [Relatório](docs/relatorios/2026-11-17-core-create.md) com addendum datado; repo `core-create main` = `2d438c9` |

## Próximo trabalho

| Prioridade | Pendência | Condição / evidência necessária |
| --- | --- | --- |
| 1 | Item 9: captura completa de estado, save/replace/remove/export | Revalidar código actual. D63 foi sugestão; política final por operação não está ratificada |
| 1 | Proposta inteira tudo-ou-nada e isolamento AI | Fronteira define alvo; bridge/painel ainda ligados no snapshot auditado. OFF e ai_boundary_static não demonstrados |
| 1 | Item 6: nomes reservados Windows no export Bitwig/MIDI | Texto já recebido: CON/NUL podem produzir nome inválido; revalidar bitwig.cpp/midi.cpp e testar. Não executado em Windows |
| 2 | Remover escolhas de instrumentos próprios, Kick Design/Cymbal Design, módulos antigos e samples distribuídos | Direcção decidida; inventário e implementação pendentes, preservando projectos/padrões |
| 2 | Explorar painéis de Tools e limpar menus | Chain por pistas/intervalos; abrir editor de cada instância; desenho final por aprovar |
| 2 | Preservar Pattern Maker e alinhamento pratos/kick/baixo | Provas de regressão antes de retirar módulos |
| 3 | Formatos adicionais de plugins | Preparar matriz; não prometer suporte universal |
| 3 | Contrato docs/contracts/core-calls.md | Ausente no snapshot auditado; distinguir legado de contrato alvo |
| 3 | Curadoria do core-create | `psytrance` não tem entrada em `pcore/styles.json` nem template; decidir se entra no catálogo ou sai dos padrões. Falta a tua decisão do **formato**: por agora o repo é só markdown, 0 JSON (instrução de 17/11); quando o formato for decidido, o gerador volta a emitir. Decidir também se algum lado passa a ler `core-create` (D50) enquanto o core-main continua a fonte da app |
| 3 | Control itens 19–25 e variantes dos bugs | Revalidar e classificar Core/AICORE pela fronteira, sem mover código automaticamente |

## O que não está provado

- Runtime Windows dos itens 7, 8, 15, 18, 27, 28 e 31 permanece não medido segundo o audit.
- Nenhum teste do produto foi corrido nas entregas documentais desta sessão.
- Independência com pacote AI ausente, pipes AI e compatibilidade de formatos adicionais não estão demonstrados.
- Cache: audit reporta hash inicial de 64 KiB e revalidação parcial WAV; tail/hash total foram sugestões, não implementação comprovada.
- Não há workflow GitHub na árvore consultada. Gates locais existentes: ci/gate.sh e ci/gate.ps1. “Tier1/Tier2” não são critérios executáveis definidos aqui.

## Decisões e histórico

Produto PROD-01…PROD-10: pedidos explícitos registados. Fronteira D-AI-fora/A09/D-DEFER1/D47: conforme instrução do dono; desvios actuais identificados no documento.
A folha D58–D66 do coordenador não foi adoptada integralmente. Port/fecho do PR #25 e arquivo do core-main não foram executados nem autorizados por esta limpeza.

Relatórios são históricos e ligados ao seu SHA; não substituir os seus números por resultados novos. Ao concluir trabalho, actualizar esta página e acrescentar relatório no [índice](docs/relatorios/README.md).
