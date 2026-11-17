# ESTADO do Core — qwen-core

Última actualização: **2026-11-17**, às 21:00 (hora desta caixa), sobre `main` = `7741a4e`.

## Relatório

O detalhe todo está em **[`docs/relatorios/2026-11-17-auditoria-core.md`](docs/relatorios/2026-11-17-auditoria-core.md)**
— o que foi corrigido, item a item, com a prova vermelha e a verde de cada fornada; o que foi medido e o que
**não** foi medido; o que falta fazer; e as decisões que preciso de ti. Esta página é só o painel: não duplica
o relatório, aponta para ele.

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

## Separação Core ↔ AICORE (a tua nota de hoje)

Anotada e medida: não há modelo, chat ou prompts dentro do binário; `core_control` é só o lado receptor do
contrato (catálogo do que o Core pode chamar, validar/aplicar, ficheiros da ponte). O que hoje puxa conteúdo do
AICORE para dentro do repo são os 45 ficheiros de `patterns/` com `"source": "AICORE docs/knowledge/…"` e as
tabelas de `edit.cpp:1103`/`:1175` — fica à espera da tua decisão (e do teu ficheiro com a separação explicada)
antes de se apagar seja o que for.

## Quem faz o quê

Eu: itens abertos que não precisam de decisão, e as variantes do mesmo defeito que aparecem ao lado de cada
item corrigido. Tu: decisões numeradas da secção 5 do relatório, o texto do item 6, e o ficheiro da separação
Core ↔ AICORE.
