# ESTADO do Core — qwen-core

> **Nova clarificação do dono:** ficam apenas **Pattern Maker e Tools** como ferramentas criativas próprias.
> **Tools por explorar:** painéis simplificados com abertura directa do editor de cada instância da chain. Remover Kick Design/Cymbal Design. Suporte de plugins extensível a vários formatos; não prometer compatibilidade universal já entregue.
> Preservar catálogo de padrões e alinhamento de pratos/kick/baixo; som por plugins externos. Retirar módulos antigos, samples distribuídos e menus redundantes.
> [Direcção e plano de limpeza](docs/DIRECCAO-CORE-PATTERN-MAKER-TOOLS.md). **Documentado; ainda não implementado.**

> **FRONTEIRA DO DONO — AI FORA POR AGORA.**
> Ler [FRONTEIRA-CORE-AICORE.md](docs/FRONTEIRA-CORE-AICORE.md) antes de mexer em dependências, processos, persistência ou âmbito.
> Core: DAW, catálogo musical, validação integral e Undo. Pacote externo: modelo, chaves, memória/SQLite, chat/JSONL e bridge AICORE.
> **Não mover/apagar código ou dados sem decisão numerada.**
> A bridge/painel ainda estão ligados no snapshot `599d558`; `CORE_BUILD_AI=OFF` e `ai_boundary_static` são requisitos ainda não confirmados neste repo.

O restante painel conserva os números e pendências do audit anterior; não são resultados reexecutados pela entrega documental da fronteira.

Última actualização: **2026-11-17**, às 21:00 (hora desta caixa), sobre `main` = `7741a4e`.

## Relatório

O detalhe todo está em **[`docs/relatorios/2026-11-17-auditoria-core.md`](docs/relatorios/2026-11-17-auditoria-core.md)**
— o que foi corrigido, item a item, com a prova vermelha e a verde de cada fornada; o que foi medido e o que
**não** foi medido; o que falta fazer; e as decisões que preciso de ti. Esta página é só o painel: não duplica
o relatório, aponta para ele.

Para ler em cinco minutos: **[`docs/relatorios/2026-11-17-RESUMO.md`](docs/relatorios/2026-11-17-RESUMO.md)**.

Pasta de relatórios: [`docs/relatorios/`](docs/relatorios/). Um ficheiro por dia/assunto, sempre começados por
"o que foi medido".

## Números de agora (medidos nesta caixa, Linux, sem JUCE)

| | |
|---|---|
| Gate `ctest -L merge` | **15 testes, 0 falhas** |
| Casos / verificações | **113 / 1456** |
| Estado do audit de 2026-11-17 (31 itens) | **21 corrigidos** (1, 2, 3a, 3b, 4, 5, 7, 8, 10-15, 18, 22, 26, 27-31) · **10 em aberto** (6, 9, 16, 17, 19, 20, 21, 23, 24, 25) |

O número de casos nunca desceu em nenhuma etapa de hoje: 88/1288 na triagem dos testes herdados → 104 → 111 →
113.

## O que está em aberto, em uma linha cada

- **6** — o texto do item nunca me chegou; preciso dele para o verificar.
- **9** — captura de estados de plugin aceita respostas com chaves em falta; falta `pluginStateKeys` no domínio
  e a recusa na app. Política de timeout (5000 ms) é decisão tua.
- **16, 17** — UI e som: "Core's own instruments" nos menus e "Randomize sound" activo sem nada para
  randomizar. Com D46 ambos cheiram a mal colocados; não mexo sem o teu aval (D50).
- **19, 20, 21, 23, 24, 25** — control / AICORE. A tua regra de hoje (AICORE fora do binário, é add-on) muda o
  enquadramento: o verificador está a re-corres e separa o que é lado Core do que é lado AICORE.
- **Windows não validado** — itens 7, 8, 15, 18, 27, 28, 31: código corrigido e compilado; nada correu. Há
  um plano de teste para cada um no relatório.

## Separação Core ↔ AICORE — clarificação do dono

A referência é [docs/FRONTEIRA-CORE-AICORE.md](docs/FRONTEIRA-CORE-AICORE.md). Substitui a interpretação anterior de que toda a `core_control`, incluindo `bridge.cpp`, teria de permanecer no Core.

Catálogo, validação tudo-ou-nada, transacção e Undo são do DAW. A bridge específica AICORE, painel/chat, modelo, chaves, conhecimento e memória pertencem ao pacote externo. A integração futura usa pipes locais, sem sockets/rede. A implementação actual usa ficheiros e ainda precisa de separação física; nada foi movido nesta tarefa.

Leitura individual dos patterns: **45 JSON no total, 39 com proveniência AICORE e 6 com outras fontes**, todos `hypothesis`. A origem não autoriza apagar dados nem prova uma dependência em runtime; destino é decisão do dono.

As referências D-AI-fora, A09/D-DEFER1 e D47 vêm da instrução actual do dono. A PR #4 não autoriza painel/memória no Core. O retorno da AI exige decisão numerada e gates verdes, incluindo Core sem pacote instalado.

## Quem faz o quê

Eu: itens abertos que não precisam de decisão, e as variantes do mesmo defeito que aparecem ao lado de cada
item corrigido. Tu: decisões numeradas da secção 5 do relatório, o texto do item 6, e as decisões de implementação pendentes na fronteira Core ↔ AICORE (o documento já está escrito).
