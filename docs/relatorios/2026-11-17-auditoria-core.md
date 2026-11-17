# Relatório — audit "ARREDURA PROFUNDA DO CORE", 2026-11-17

> **Clarificação posterior do dono (17-11-2026):** a arquitectura aplicável está em [FRONTEIRA-CORE-AICORE.md](../FRONTEIRA-CORE-AICORE.md).
> A frase da secção 5.1 “isso tem de ficar no Core” **não autoriza manter bridge.cpp/painel/memória no binário**: só catálogo musical, validação integral, transacção e Undo ficam do lado DAW. AI continua diferida.
> O inventário posterior encontrou 45 patterns no total, **39 com origem AICORE e 6 de outras fontes**.
> O relato e as medições abaixo são preservados como histórico; esta nota não declara novo teste verde nem alteração de código.

Repo: `dmo-app/qwen-core`, rama `main`. Este ficheiro é o registo do que foi feito, do que foi medido, e do
que ficou por resolver. A página [`ESTADO.md`](../../ESTADO.md) aponta para aqui, e
[`2026-11-17-RESUMO.md`](2026-11-17-RESUMO.md) é a versão de cinco minutos.

Origem: auditoria do Diogo de 2026-11-17 sobre o snapshot `95f411a`, 31 itens. Cada item foi re-verificado
contra o código actual antes de se mexer nele (a auditoria foi escrita sobre um snapshot mais velho, e alguns
itens já tinham caído pelo caminho). Autorização de trabalho: *"vou te criar um repo novo para poderes fazer
commits à vontade"* e, para os testes/CI herdados, *"não sigas a regra os testes ci anteriores pq o claude
fez muita merda, não se sabe o que realmente se aplica"* → nada do que veio do `core-main` foi tomado como
ligação; foi tudo verificado de novo.

---

## 1. Estado medido agora

Medido nesta máquina (Linux, sandbox), `cmake -DCMAKE_BUILD_TYPE=Release` + Ninja, sem JUCE:

| | |
|---|---|
| Testes registados no gate (`ctest -L merge`) | **15**, 0 falhas |
| Casos de teste | **113** |
| Verificações | **1456** |
| Suite por suite | audit2 5/66 · audit 7/65 · bitwig 2/25 · bug 10/49 · calls 10/44 · domain 9/64 · engine 20/81 · hunt 14/649 · midi 8/115 · role 21/216 · round3 7/82 + 4 testes `cli_*` |

Percurso dos números hoje: `5d0b584` baseline → `31b988b` triagem (14 testes, 88 casos, 1288 verificações) →
itens 10-14 → itens 22/26 (104 casos) → itens 1-5 (111/1426) → família dos nomes (113/1456). **O número de
casos nunca desceu em nenhuma etapa** — foi a regra que o Diogo pôs, e está cumprida.

### Como se trabalha aqui (regras do ledger que mandaram no método)

- **Vermelho primeiro, sempre no estado anterior.** Cada fornada tem dois commits: um só com testes, medido
  sobre uma cópia limpa do commit de antes (`git archive <sha> | tar -x`, build em pasta própria), e outro com
  a implementação. As linhas `FALHOU ... medido X, esperado Y` vão no texto do commit.
- **Nada é apagado sem decisão numerada.** Não cortei ficheiros, não desabilitei testes, não mudeu limites do
  leitor por conveniência.
- **Flaky = bug.** O gate foi corrido 3 vezes seguidas em cada fornada; nada variou.
- **D46** (Core = DAW leve, som só de VST3) e **D50** (arquitectura e UI precisam de aval explícito do dono):
  nada foi decidido sozinho nesta área; o que depende disso está na secção 5 como pedido, não como feito.
- **Linux ≠ Windows.** A app de desktop (`app/desktop`) só é adicionada ao build quando `WIN32`. Aqui foi
  compilada numa árvore de trabalho com a barreira removida; não correu.

---

## 2. O que foi corrigido

Dos 31 itens: **21 corrigidos** (1, 2, 3a, 3b, 4, 5, 7, 8, 10, 11, 12, 13, 14, 15, 18, 22, 26, 27, 28, 29, 30, 31) e **10 em aberto** (6, 9, 16, 17, 19,
20, 21, 23, 24, 25). Dos 21, sete são código corrigido e compilado sem runtime medido (7, 8, 15, 18, 27, 28, 31)
— está marcado abaixo e na secção 3.

### Corrigido e medido em Linux (vermelho → verde nos testes do repo)

| Itens | O que estava mal | Commit |
|---|---|---|
| 10, 11, 12, 13, 14 | O cabeçalho do WAV passou a ser interpretado e validado antes de se tocar no áudio (`parseWavHeader` + a nova `wavInfo`): ficheiro malformado com o limite em 1 MiB custa 6 396 009 → 9 489 bytes lidos, e ler ao ritmo do projecto passou de 8 533 586 → 4 542 070 porque o `resample` ao mesmo ritmo devolvia uma cópia; `freeze` mais curto do que um grão; `adopt` no tempo certo das duas escalas e nota-offs reservadas; o dispositivo que está a tocar posto de parte num `engine.start` falhado | `f89ed11` + `360bdb7` |
| 22, 26 | No `validateProposal` do control a **última** faixa com o nome pedido ganhava (o resto do Core escolhe sempre a primeira, e o `id` não batia antes do nome); e uma chamada `Mix` sem botões era aceite, aplicada como passo de undo e dizia "Mix on Kick" sem tocar em nada | `d342928` + `cf4f12d` |
| 1, 2, 3a, 3b, 4, 5 | Nomes de padrões do randomizador acima dos 40 bytes do leitor e com caracteres partidos; MIDI que prometia 2 pistas e trazia 1 era aceite calado; índice da biblioteca e cache do motor reusados por caminho+tamanho+data (ficheiro trocado por dentro ficava com o `sha`/nível velho e o áudio velho); `duplicateSection` esquecia os pontos de tempo dentro da secção; `toolSuck`/build-up/fx-por-intenção escreviam meia canção antes de recusar por cadeia cheia | `2dc9353` + `91a2836` |
| 1 (família) | O mesmo buraco dos nomes por outros caminhos: o dialog "Rename pattern" aceita 60 caracteres e o nome ia sem limite para dentro de um padrão (`rackPatternFor`, `duplicateRackSlot`) — `saveFile` rebentava numa canção que a app aceitou; e `duplicateTrack`/`makeClipUnique`/`duplicateSection`, que com o nome cheio davam à cópia exactamente o nome do original. A regra dos 40/80 bytes passou a existir uma vez (`cutUtf8`/`nameWithSuffix` ao lado do leitor) em vez de três cópias escritas à mão | `3146cce` + `7741a4e` |

Prova vermelha medida de cada uma destas fornadas: 15 falhas (22/26) · 17 falhas (1-5) · 9 falhas (família
dos nomes). Verde em cima: 15 testes, 113 casos, 1456 verificações, 0 falhas.

### Corrigido no código, **sem** medição em runtime (só compilação)

| Itens | O que estava mal | Commit |
|---|---|---|
| 7, 8, 15, 18 | Canção nova herdava `pluginsDirty_`/`pluginEditsSeen_`/autosave da canção anterior; `pluginEdits` voltava a zero a cada reinício do motor e marcava a canção como modificada; o mixer não oferecia On/Remove nas inserções (um efeito herdado de uma canção velha não se tirava de lá); respostas de parâmetros atrasadas do plugin errado a escrever em `roles.json` | `2ce75cd` |
| 27, 28, 29, 30, 31 | Instalador apagava ficheiros do utilizador que não tinha copiado; `--self-test` destruía `.core`/`-audio/`/`-saved/` vizinhos; recursos (`templates`/`lang`/`patterns`/`pcore`) sem dependência real, por isso builds incrementais entregavam recursos velhos; gate com cache `CORE_BUILD_TESTS=OFF` dava VERDE com zero testes (medido: `rc=0` → com a correção `rc=8`); `-DFETCHCONTENT_SOURCE_DIR_JUCE` partido por espaços em `gate.ps1` | `6d2503b` |

Destes, **29 e 30 foram mesmo medidos** em Linux: 29 com Ninja numa árvore de ensaio (editar só
`pcore/styles.json` volta a executar a cópia; um `STALE.json` posto à mão na pasta da app desaparece; rebuild
sem alterações é no-op) e 30 com o mecanismo do `ctest` reproduzido à mão. **27, 28 e 31 não**: são
Windows-puro, ficaram por leitura de código (27 com simulação do raciocínio em Python), e precisam do PC do
Diogo para se provarem. O `2ce75cd` diz "os 23 TUs de app/desktop": o número medido é **21 de 23** — os dois
que não compilam fora de Windows (`Autosave.cpp`, `Diagnostics.cpp`, `juce::File::windowsLocalAppData`) já
vinham de antes e não foram tocados aqui. Fica registado como correcção de honestidade, sem reescrever o
histórico.

---

## 3. O que não foi medido — dito à maneira da regra da honestidade

- **Nada da app de desktop correu.** Itens 7/8/15/18/28/31: o comportamento (o "por guardar" que deixa de
  aparecer, o menu do mixer ao rato, o `--self-test` seguro, o caminho do JUCE com espaços) é inferido do
  código e da compilação, nada é observado.
- **3a é uma hash dos primeiros 64 KiB.** Uma substituição dentro do lugar que mude só bytes depois dos 64 KiB,
  mantendo tamanho e data, ainda não é apanhada. **3b revalida só o cabeçalho do WAV**: um ficheiro com os
  mesmos canais/quadros/taxa/data mas amostras diferentes continua servido do cache. Custo: ≤ 64 KiB por
  ficheiro em cache por varredura, e uma leitura de cabeçalho por construção de grafo.
- **O índice da biblioteca ficou na versão 2** (de propósito): o leitor aceita 1 e 2, e subir a versão deitaria
  fora o índice de todas as bibliotecas já varridas. Um índice velho sem `headSha256` é adoptado à primeira
  varredura.
- **Os testes do control e do domínio não cobrem a app.** Onde um item é da app, prova-se que a função de
  domínio devolve a coisa certa, não que o utilizador vê a coisa certa.
- **O gate de Windows (`ci/gate.ps1`) não corre aqui** — não há `pwsh` nem `cmd.exe` na caixa.

---

## 4. O que falta fazer

**Sem dependência de ti (tomo eu):**

1. **Item 9** — o Core captura o estado dos plugins e aceita-o mesmo quando faltam chaves; e o `removeEffect`
   ignora por completo o resultado da captura (um undo depois de um timeout traz o preset como foi guardado, não
   como soava). Falta `pluginStateKeys(const Document&)` ao lado de `hasPlugins`/`applyPluginStates` em
   `app/domain/edit.cpp` e uma recusa explícita no `fetchPluginStates()` quando as chaves esperadas não estão
   todas lá. A parte de domínio é testável em Linux; a parte da app não.
2. **Itens 19, 20, 21, 23, 24, 25** (control / AICORE) — o verificador está a re-corres por causa da regra nova
   da separação Core ↔ AICORE (secção 5). Assim que o mapa voltar, ou são corrigidos aqui (lado Core do
   contrato) ou viram nota para o repo `dmo-app/AICORE`.
3. **Variantes do mesmo defeito que ainda não foram apanhadas.** Já apareceram duas vezes: um item corrigido e
   o verificador encontrar o irmão logo ao lado (o `patternName` do randomizador → o rack; o `toolSuck` →
   `toolIntentFxTracks`, que ainda pode escrever numa faixa e recusar noutra, se o caminho não for o que o
   `requireChainRoom` cobre). Vou continuar a procurar o irmão de cada item antes de o fechar.

**Com dependência de ti:** secção 5.

---

## 5. Decisões e avisos que preciso de ti

1. **AICORE fora do binário** (a tua nota de hoje). Medido: não há modelo, chat, prompt ou guia dentro de
   `app/` ou `engine/` — `grep` por `prompt|api_key|openai|anthropic` não devolve nada, e
   `MainComponent.cpp:3524` diz "the AI lives in AICORE"; o add-on é descoberto por `addon.json` na pasta que
   escolheres (`bridge.cpp:204`), e o instalador não empacota AICORE nenhum. O que está dentro do binário é
   `core_control` (`calls.cpp` + `bridge.cpp`), que é o **lado receptor** do contrato: o catálogo do que o Core
   sabe chamar, o validar/aplicar de uma proposta, e os ficheiros da ponte. Isso tem de ficar no Core.
   **O que puxa AICORE para dentro hoje:** os 45 ficheiros de `patterns/` com
   `"source": "AICORE docs/knowledge/…"` e `"status": "hypothesis"`, e as tabelas de `edit.cpp:1103`/`:1175`
   com a mesma proveniência. É **decisão tua e com número**: são dados do Core (o AICORE sugeriu, a origem fica
   como comentário) ou é conteúdo do add-on que tem de sair deste repo? Não apago nada sem ela.
   Fico à espera do teu ficheiro com a separação Core ↔ AICORE; quando existir, marco aqui as linhas que ele
   alterar.
2. **Item 6 — não tenho o texto.** Está na tua auditoria e nunca me chegou (nem no ficheiro, nem na conversa).
   Sem texto não o posso verificar nem corrigir: se me colares o item 6, entra na fornada a seguir.
3. **Item 16** — "Core's own instruments" continua nos menus (adicionar/substituir). Com D46 o som não vem do
   Core, por isso escolher isso cria uma faixa silenciosa. Mexer num menu é UI: preciso do teu aval para os
   tirar (os dados velhos ficam a ler-se, só desaparecem das escolhas).
4. **Item 17** — "Randomize sound" fica activo quando existe um estilo, havendo ou não instrumentos nativos, e
   depois rebate. Com D46, a própria acção pode estar mal colocada: ou desliga quando não há nada para
   randomizar, ou passa a trabalhar sobre os plugins dos papéis. É decisão de arquitetura/UI.
5. **Item 9, política de timeout** — a captura de estados de plugin tem 5000 ms. Se um plugin estiver a demorar,
   queres recusa com aviso, espera maior, ou aplicar o que veio e dizer o que falta?
6. **`docs/contracts/core-calls.md` não existe**, mas é citado por `calls.hpp` e por `bridge.hpp` como o
   contrato do bridge. É o contrato que o AICORE lê. Escrevo-o eu (no lado Core, porque é o Core que publica o
   estado) ou vem no teu ficheiro da separação?
7. **CI deste repo.** O `.github/workflows` não foi importado para `qwen-core` (só `ci/gate.sh` e
   `ci/gate.ps1` cá estão). Reconstruir o CI precisa de um token com scope `workflow`; diz se quero que o faça e
   com que label de gate (`merge` já existe e é o que corro aqui).
8. **`core-main` PR #25 e D58** — continuam pendurados do outro lado: fechar ou mesclar o PR, e ratificar D58.
   Nada disto toca `qwen-core`.
9. **Custo da varredura da biblioteca** (3a): ≤ 64 KiB lidos por ficheiro em cache por varredura, em troca de
   deixar de servir registos velhos de ficheiros trocados por dentro. Aceitas este preço, ou prefires que a
   cabeça hash seja maior (mais seguro, mais lento)?

---

## 6. Como reproduzir as medições

```sh
# verde de hoje
cmake -S /data/qwen-core -B /tmp/gate -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCORE_BUILD_APP=OFF -DCORE_BUILD_TESTS=ON
ninja -C /tmp/gate && ctest --test-dir /tmp/gate -L merge --output-on-failure

# vermelho de uma fornada: o estado ANTERIOR + só os testes novos por cima
git -C /data/qwen-core archive <sha-do-estado-anterior> | tar -x -C /tmp/red
cp <os-testes-da-fornada> /tmp/red/tests/
cmake -S /tmp/red -B /tmp/red-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCORE_BUILD_APP=OFF -DCORE_BUILD_TESTS=ON
ninja -C /tmp/red-build hunt_tests && /tmp/red-build/tests/hunt_tests
```

Os comentários de cada commit levam as linhas `FALHOU` exactas do vermelho correspondente.
