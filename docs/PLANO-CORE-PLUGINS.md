# Plano: Core como DAW básica com plugins

> **Referência histórica do snapshot indicado abaixo.** Direcção e pendências actuais: [ESTADO](../ESTADO.md) e [índice](README.md). Não constitui autorização para reintroduzir funcionalidades antigas.

Atualizado a 2026-11-16. Base: o `main` em `d4c483c` (PR #3, depois do PR #2 `a8c0dcc`).

## A ideia

O Core passa a ser uma DAW simples, feita à maneira do Diogo. As notas, a estrutura e as tools são do Core; o som vem todo de plugins (VST3 agora, VST2 depois). O Bitwig deixa de ser obrigatório e fica só como opção, por export `.mid`.

## Feito

### 1. Os 9 bugs graves (levantamento de 2026-11-14), commit `8e816c1`

| Id | Bug |
|---|---|
| D1 | Remover o loop deixava a música impossível de gravar (e o autosave falhava) |
| D2 | Trocar para Sampler gravava a versão 3 e o Save recusava |
| D4/D6 | Nomes cortados a meio de um carácter (bounce, secção): UTF-8 inválido, Save falhava |
| ED1 | Send to Arrange lia memória já libertada |
| P1 | Randomize (ghost kick) punha notas fora do padrão: a música não voltava a abrir |
| PL1 | F-Core em modo nota passava os 16 moduladores: a música não voltava a abrir |
| PL2 | Conversão Core FX com fxOnly=1: pista muda fora das regiões |
| PL3 | F-Core sobrevivia à troca de instrumento: o motor recusava o projeto |
| E1 | `engine.cpu` usava o endereço do medidor como identidade: leitura fora dos limites |

Verificado: compila, self-test 53/53, e verificação à parte de D1, D2, D6, PL1, PL3.

### 2. O bug do knob, explicado (commits `b507478`, `1e5cdc9`)

`core-engine knobcheck` prova o caso: toca a música offline, mexe num knob (por `project.load`, como a app), repõe-o e compara com a mesma música sem toques. Uma linha JSON por knob. Antes disso verifica que duas corridas iguais dão amostras iguais e que recarregar a mesma música não muda o som (e diz em que pista muda). Com a saída do som próprio, os knobs desses instrumentos deixaram de existir.

### 3. Fora o som próprio (PR #2, commits `8769e88`, `67a140c`, `74fbf70`)

- Saíram W-Core, F-Core, R-Core, kick, hats, snare, sampler, Sample Path, Glitch, Core FX/EQ, LFO e moduladores, racks, devices e a cadeia de efeitos própria (cerca de 30 mil linhas a menos).
- O motor regista só os tipos `plugin`, `audio`, `bus` e `none` (silencioso até ter um plugin).

### 4. Anfitrião VST3 (commits `14ead77`, `3b203b4`, `23f75c8`)

- **Plugins > Scan plugins (VST3):** cada ficheiro é lido e testado no seu próprio processo; os que crasham ou dão NaN ficam de lado.
- **+ Track:** instrumentos VST3 por fabricante. Menu da pista: Instrumento > VST3, e Plugin effects (abrir, ligar/desligar, remover, adicionar). Efeitos por pista e no master.
- **Janela do plugin:** duplo clique abre; mexer na janela marca a música como por gravar.
- **Estado guardado na música:** o estado de cada plugin vai para o `.core` ao gravar e ao exportar. Automação `vst:`, e relink de plugins entre PCs (`plugin_refs`).
- Verificado: Serum 2 na pista e ValhallaDelay como efeito carregam e tocam (pico da pista 0.14), sem plugins em falta.

### 5. Templates e músicas antigas abrem

- Uma pista com instrumento próprio passa a `none` com uma nota, e mantém as notas à espera de um plugin. Os efeitos próprios ficam guardados e silenciosos.
- A automação de um instrumento que saiu é largada com nota; a de mistura (`gainDb`, `pan`) e a dos efeitos (`fx:`) ficam guardadas na música (commit `131f1b5`). Só tocam a de mistura e a dos efeitos plugin (`fx:<id>:vst:...`); as dos efeitos próprios ficam caladas, como os efeitos.
- Os 21 templates em `templates/` abrem e o motor toca-os (testado em `tests/domain_tests`). Tocam as notas e a mistura: os 1108 efeitos próprios (`builtin`) e os 24 moduladores dos templates ficam guardados no `.core` e fora do motor até haver um plugin no lugar deles (caça profunda P04, `tests/hunt_tests`).

### 6. Testes e CI (PR #3)

- `tests/engine_tests`, `tests/domain_tests`, `cli_version`, `cli_demo_render`, `cli_measure` (label `merge`) e `desktop_self_test` (label `desktop`). Regras em `tests/README.md`.
- Gate de um comando: `ci/gate.ps1` (PC do Diogo) e `ci/gate.sh` (Linux/WSL).
- Workflow `.github/workflows/ci.yml` (Linux e Windows). **Nota:** as 3 corridas no GitHub falharam em 2 segundos, antes de qualquer passo, nos dois sistemas. Parece um problema da conta/runners do GitHub Actions, não do código; está por ver.

### Formato `.core` (sem `.dawproject`)

O `.core` guarda as pistas, os clips, as secções, o tempo, a automação e o estado de cada plugin. O Core abre e toca isso sozinho.

## Feito a 2026-11-16 (ramo `claude/project-thread-rl6ita`)

### 7. Mapa de papéis (passo 1)

- **Settings > Roles:** um plugin por papel: efeitos (reverb, delay, filter, sidechain, drive) e instrumentos (bass, lead, pad, pluck, drums). Guardado em `%APPDATA%\Core\roles.json`, fora das músicas.
- Ao escolher o plugin, o Core lê os parâmetros dele (`core-engine params <ficheiro> --id <id>`, no seu próprio processo) e adivinha as macros pelo nome (mix, size, cutoff, volume...). "Parameters..." muda qualquer uma.
- Domínio sem JUCE (`app/domain/src/roles.hpp`), para as tools e para a AI: `ensureRoleEffect`, `roleParamKey`, `addRoleTrack`, `setTrackRole`. O efeito fica marcado com `"role"` na música.

### 8. Tools pelos plugins dos papéis (passo 2)

- **Piano roll > Plugin tools:** SUCK para o drop (1, 2 ou 4 compassos), PUM a partir de outra pista (compassos mostrados ou a música toda) e efeitos do build-up (filtro + reverb, + delay, + drive).
- **Painel do build-up:** linha "Effects", no mesmo passo de undo que as notas.
- Tudo é automação normal dos plugins (`app/domain/src/role_tools.hpp`): undo, gravar e exportar funcionam como sempre.

### 9. MIDI (passo 4)

- **File > Import MIDI:** cada pista do ficheiro vira uma pista sem instrumento, com padrões de 4 compassos (compassos iguais são o mesmo padrão).
- **File > Export MIDI:** a música como toca, com os rolls expandidos como no motor, uma pista MIDI por pista e o tempo com as rampas.

### 10. Bugs médios e pequenos (passo 6, 1.º lote)

Da lista revista do sweep (`sweep-qwen-revisto.md`, 45 por corrigir). Corrigidos, com teste em `tests/bug_tests.cpp` ou `tests/engine_tests.cpp`:
`engine.start` recusado já não pára o som; loop convertido duas vezes numa mudança de tempo; `setLoop` com início negativo; `measure` "filter" media silêncio; `readWav` com chunk 0xFFFFFFFF; `isolateRegion` apagava `extra` e cortava o nome a meio de um carácter; STUTTER ignorava as repetições de um clip em loop; FOLLOW não gravava o corte; `addPattern` com comprimento 0; ids enormes (`strtol` + 1); `schemaVersion` 2^32+1; `nextSlotId` com `stoi`; `approval` dos padrões; id de padrão com sufixo > 80; casts do P-Core; `noteTool("random")` sem pista; colar áudio no fim não fazia crescer a música; `copyClips` com o BPM inicial.

### Auditoria externa (2026-11-16, `development\bug\RELATORIO-BUGS-MELHORIAS.md`)

Corrigido, com testes (`tests/audit_tests.cpp`, `tests/cli_unicode.cmake`; cada um falhava antes):
- B01: importar `take..1.wav` já não impede guardar nem o autosave (o nome gerado não leva `..`; a validação recusa só uma parte `..` do caminho).
- B02: uma pista nunca fica com o id `master`; ficheiros antigos com essa pista abrem com ela renomeada (os estados de plugin já não se misturam com os do master).
- B03: recortar um template, ou apagar compassos, a meio de uma rampa de tempo guarda a rampa.
- B04: exportar em mono mede o sinal mono (já não recusa um clipping que só existe em estéreo; o pico devolvido é o do ficheiro).
- B05: a linha de comandos lê e escreve caminhos Unicode no Windows (argumentos em UTF-8).
- B06: `render --end-tick 0` é recusado como região vazia, como no `render.start`.
- R01: se o motor não der os estados dos plugins a tempo, Guardar avisa, a música continua por guardar e Fechar não avança.
- R02: o autosave pede primeiro os estados dos plugins mexidos na janela do plugin (sem bloquear).
- R03: guardar como template captura antes os estados dos plugins.
- R04: exportar pára a pré-escuta do Patterns, para exportar a música e não o slot.
- R05: abrir a janela de um plugin (ou ler os parâmetros) envia antes a edição pendente.

D01 (o `render` da linha de comandos aceita áudio em falta): fica como está, a linha de comandos não faz parte da ligação ao Bitwig (Diogo, 2026-11-16).

### Segunda auditoria (2026-11-16, sweep por análise de código)

Verificado e corrigido, com testes em `tests/audit2_tests.cpp`:
- Bug 1 (confirmado): uma resposta do motor com `error` ou `ok` de outro tipo lançava dentro da thread de leitura do cliente, e isso fecha a app. Agora cada linha é lida sem lançar (`engine_protocol.hpp`), e qualquer falha na leitura termina a sessão do motor de forma limpa (os comandos pendentes falham com o motivo e o motor reinicia).
- Bug 2 (confirmado): depois de um `project.load` sem resposta a tempo, nada voltava a ser enviado até à edição seguinte. Agora volta a tentar sozinho (0,5 s, 1 s, 2 s, 4 s, 8 s) com uma revisão nova, porque o motor pode ter aceitado a antiga tarde. Recusas e mensagens grandes demais continuam à espera da próxima edição.
- Bug 3 (confirmado): o espaço em disco do export contava todos os ficheiros como estéreo e só olhava para a pasta do primeiro. Agora cada ficheiro conta com os seus canais e cada volume é verificado com a soma do que lá vai.
- Ponto 4 (versão aprofundada, confirmado): uma resposta do motor sem `id` válido ficava só no registo e o comando à espera só falhava no timeout (10 s). Agora a sessão do motor termina logo, com o motivo, e o motor reinicia (o cliente manda sempre ids inteiros, por isso uma resposta assim é um protocolo partido).
- Observação do autosave: não era bug (quem chama apanha o erro e o autosave estragado nem aparece na lista), mas a mensagem passou a dizer "The autosave is damaged" em vez do erro do JSON.

### Caça profunda (2026-11-16, `development\bug\audit-profunda\RELATORIO-CAÇA-PROFUNDA.md`)

Corrigido, com testes em `tests/hunt_tests.cpp` (PR #7 e PR #8):
- P01: o bounce corta o nome antes de " (bounce)" sem partir acentos; as mensagens de limite dizem "bytes".
- P02: `render` (CLI) e `render.start` (app) usam a mesma cauda por omissão, 2000 ms. **Atenção:** um `render` da CLI sem `--tail-ms` passou a dar ficheiros 0,5 s mais compridos do que antes de `e9b1707` (era 1500 ms). Comparar WAVs antigos com novos mostra essa diferença, que é só cauda.
- P03: ids de efeito repetidos ou com `:` são limpos ao abrir, como o motor faz; o estado do plugin já não se perde.
- P04: não é bug (o som vem só de plugins); o texto do plano acima foi corrigido.
- P05: a conversão de taxa dá o número certo de frames (44,1 kHz para 48 kHz: 48000, não 48001). O pico ligeiramente acima de 1,0 depois do resample fica: é o filtro, e o export recusa o que passa de 0 dBFS (B04).
- P06: apagar a pista mostrada mostra outra selecionada ou a vizinha, nunca a primeira acrescentada à seleção.
- P08: um clip só de NaN/Inf diz "ficheiro estragado" no Cut silence, em vez de "silêncio".
- P10: 2 textos traduzidos e "Save as template" com uma só grafia.
- IPC: uma resposta com um campo `event` continua a ser resposta (antes esperava o timeout).
- §4: `transport.ppq` diferente de 960 diz "must be 960", e não "between 960 and 960".
- Logs: ficam 20 de cada tipo contando com o da sessão (eram 21). Os 14 .dmp de 4 a 10/11 ficam porque são menos de 20 (como desenhado).

### Ronda 3 do audit (2026-11-16, `development\bug\audit-profunda\ROUND3-PR9-PR4AI.md`)

Corrigido, com testes em `tests/round3_tests.cpp`:
- R3-A: `toolRoll` trabalha numa cópia da pista: numa pista com 64 padrões recusa sem cortar nenhum clip.
- R3-B: um cartão cinzento já não abre a tool (diz que plugin falta); o W-Core mostra cinzentos os papéis sem plugin; a frase do F-Core só diz o que escreveu (impact sem drive não fala de "grit").
- R3-C: 28 textos do painel Tools e dos menus traduzidos; as frases feitas de duas partes são traduzidas parte a parte.
- R3-D: R-Core/D-Core com um plugin sem macro de tamanho (feedback) diz que o tamanho é o do plugin (`sizeSet`).
- R3-E: uma pista que não pode ir para o bus (8 sends) fica como estava e a frase diz o nome e a razão (`skipped`); quem já tinha um R-Core muda sem o perder.
- R3-F: `kind` (R-Core/D-Core) e `style` (ROLL) aceitam maiúsculas, como preset, intent e section.
- R3-G: só contam as secções em que o nível do bus muda; "quieter in the drops" só aparece quando há um drop.

Não mudado: P07 (o autosave tem um slot por sessão e é apagado quando a música fica guardada ou descartada: serve para recuperar de um crash, não para guardar versões) e a versão do esquema escrita conforme o conteúdo (2 sem mudanças de tempo, 3 com elas: de propósito, para os ficheiros antigos continuarem a abrir em versões anteriores).

### Export para o Bitwig (2026-11-16)

File > Export for Bitwig: uma pasta "<música> (Bitwig)" com o .mid (uma pista MIDI por pista do Core, rolls expandidos, tempo), um .vstpreset por plugin (instrumento e efeitos de cada pista e do master) com o som atual, tirado do plugin vivo no motor (`plugin.preset`), e "Bitwig - read me.txt" a dizer que preset vai em que pista. No Bitwig: arrastar o .mid, depois largar cada .vstpreset na sua pista. Testes: `tests/bitwig_tests.cpp`; no Windows, `core-engine params <plugin.vst3> --preset-out x.vstpreset`.
O D01 (render da linha de comandos tolera áudio em falta) fica como está: não faz parte da ligação ao Bitwig (Diogo).

## Falta fazer

- **Bugs ainda abertos da lista do sweep:** gesto de undo aberto quando as linhas se refazem a meio de um arrasto; Reverse / Normalize / Cut silence no thread da UI; Align dos hats não idempotente; `swingNote` em notas pares; `setModulator` perde `source`; `sliceAudioClip` e `insertTime` com cortes < 1 ms; `scoreArrange` com secções não alinhadas; `importProjectV2` com pistas de áudio; `placeBounce` parcial; `readFolder` fora do try; nomes `.saving-N`; `applySuggestedSamples`; `bassFollow` no kick seguinte; `stretchPreset`; `humanizeNotes` entre compiladores; motion "random" em leads; duplo clique no Patterns window; estado "a tocar" falso; autosave no fecho; fecho lento; `boxClips_`; `adoptFile`; `liveFallback` (por decidir).
- **VST2:** já não é preciso (o Serum 2 original é VST3).
- **Presets do Serum por nome:** retirado pelo Diogo.

### Por juntar / por confirmar

- **Ramo `feat/ai-core`** (3 commits de 2026-11-16, não estão no `main`): chat da AI do Core (menu AI, "Aplicar" = um passo de undo), módulo `core_ai` e chamadas da música (pista, P-Core, forma, arranjo, tocar) com testes. Saiu do `main` antes do PR #3 e traz a sua própria pasta `tests/`, por isso precisa de ser atualizado e testado antes de entrar.
- **Trabalho local por enviar.** O trabalho foi parado a 2026-11-16 ~02:00. A revisão da lista de bugs médios e pequenos estava a meio no PC do Diogo e pode não ter chegado ao GitHub.
- **CI do GitHub** a falhar antes de correr (ver acima).

## Regras

- Cada passo é um commit que compila, abre sem crash e passa o self-test.
- "Se passa, mete": o que passa no CI e no "abre tudo" é juntado e instalado sem perguntar.
- A prova final é o ouvido do Diogo.

## AICORE (à parte)

App standalone em JUCE (repo `dmo-app/AICORE`). Fala com o Core só por ficheiros `.mid` e `.wav`. Áudio → MIDI com Demucs e Basic Pitch.
