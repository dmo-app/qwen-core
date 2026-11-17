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
| Criação de música por estilo → `dmo-app/core-create` | Extracção só de leitura do **CORE-MANUAL** `origin/main` = `8dc3a33` (não do core-main, como nas duas versões anteriores — ver adendum). Conteúdo = **técnicas e métodos de criação por estilo**: 4 estilos (darkpsycore 13 páginas, hitech 13, slambient 12, **psytrance 12**) + `geral/` 12 = 62 páginas de conteúdo, mais 4 de topo e o gerador = **67 ficheiros**. As 62 páginas de conteúdo são **1595 linhas / 141 KB**; o repo todo (com topo e gerador) são 1817 linhas / 155 KB de markdown. **0 JSON**. Cortes medidos: 200 frases de app, 147 ligações internas, 41 itens só app, 38 fragmentos, 15 linhas de tabela, 7 colunas, 3 tabelas reduzidas a 1 coluna, 7 secções de app, 10 itens curtos. Consumidos 374 itens de lista de 17 fontes → 518 linhas extraídas. Auditoria de nomes do Core no que sobrou: **1 ocorrência** (`psytrance/14-fichas-de-tecnica.md:49`, `STUTTER`). core-main e CORE-MANUAL intactos | [Relatório](docs/relatorios/2026-11-17-core-create.md) com três adenda datados; repo `core-create main` = `02921ea` (`eb653f8` é a extracção em si; o commit seguinte só acrescenta a conta de páginas ao `fontes.md`); versão anterior preservada na tag `antes-core-manual` (`2d438c9`) |

## Onde está o quê (medido em 17/11)

| Repo | Papel | Estado |
| --- | --- | --- |
| `dmo-app/CORE-MANUAL` | Fonte única do conhecimento musical (decisão do dono de 07/11) | `origin/main` = `8dc3a33`; lido, não escrito |
| `dmo-app/core-create` | View de técnicas e métodos de criação por estilo, extraída do CORE-MANUAL | `main` = `02921ea`; versão anterior na tag `antes-core-manual` (`2d438c9`) |
| `dmo-app/core-main` | A app e os seus dados; fonte da aplicação | `main` = `52d46c0`; não tocado |
| `dmo-app/AICORE` | Laboratório / addon do Core, fora do binário | não tocado nesta sessão |

Errei a fonte nas duas primeiras extracções (core-main em vez de CORE-MANUAL); está registado no adendum do relatório, não apagado.

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
| 3 | Curadoria do core-create | **Não medido / à espera de ti.** (1) Granulometria: 13–15 páginas por estilo com esta divisão é o que queres, ou páginas mais curtas e mais delas? (2) O guia em inglês (`knowledge/*`, capítulos do guia) ficou em inglês dentro de páginas PT — traduzir ou deixar? (3) Sub-géneros do psytrance (darkpsy, twilight, full-on, progressive) como pastas próprias em vez de uma página de perfis? (4) `14-fichas-de-tecnica.md` do psytrance tem 1 ocorrência de nome do Core (`STUTTER`) — cortá-la ou alterar a ficha no CORE-MANUAL? (5) Quem lê o `core-create` e se algum lado passa a consumi-lo (D50); enquanto não decidires, o repo é só markdown, 0 JSON, e a fonte da app continua a ser o core-main. A extracção CC-01…CC-10 da primeira versão fica ultrapassada como fonte |
| 3 | Control itens 19–25 e variantes dos bugs | Revalidar e classificar Core/AICORE pela fronteira, sem mover código automaticamente |
| 3 | Faltas de psytrance no CORE-MANUAL | Medido na folha `estilos/psytrance.md` (`8dc3a33`, 156 itens): não tem **Efeitos por papel**, **Automação**, **Faixas de referência** nem **Fora do estilo** — por isso essas páginas não existem no core-create. É lacuna da fonte, não do gerador; encher no CORE-MANUAL e regenerar |

## O que não está provado

- Runtime Windows dos itens 7, 8, 15, 18, 27, 28 e 31 permanece não medido segundo o audit.
- Nenhum teste do produto foi corrido nas entregas documentais desta sessão.
- Independência com pacote AI ausente, pipes AI e compatibilidade de formatos adicionais não estão demonstrados.
- Cache: audit reporta hash inicial de 64 KiB e revalidação parcial WAV; tail/hash total foram sugestões, não implementação comprovada.
- Não há workflow GitHub na árvore consultada. Gates locais existentes: ci/gate.sh e ci/gate.ps1. “Tier1/Tier2” não são critérios executáveis definidos aqui.
- Cobertura do core-create: **não medida**. O corte é mecânico (regras no gerador) e pode ter deitado fora técnica válida junto do texto da app; só as 17 fontes listadas em `fontes.md` do repo é que foram consumidas. Não foi lido por um produtor para validar utilidade musical.

## Decisões e histórico

Produto PROD-01…PROD-10: pedidos explícitos registados. Fronteira D-AI-fora/A09/D-DEFER1/D47: conforme instrução do dono; desvios actuais identificados no documento.
A folha D58–D66 do coordenador não foi adoptada integralmente. Port/fecho do PR #25 e arquivo do core-main não foram executados nem autorizados por esta limpeza.

Relatórios são históricos e ligados ao seu SHA; não substituir os seus números por resultados novos. Ao concluir trabalho, actualizar esta página e acrescentar relatório no [índice](docs/relatorios/README.md).
