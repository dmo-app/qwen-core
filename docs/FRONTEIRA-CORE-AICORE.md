# Fronteira Core ↔ AICORE

> **REGRA DO DONO — AI FORA DO CORE POR AGORA.**
> **Core é o DAW. AICORE é o laboratório. O futuro pacote de AI é externo e opcional.**
> **Modelo, chaves, conhecimento, memória de receitas, SQLite, chats e export JSONL não pertencem ao Core.exe.**
> **A ponte AICORE pertence ao pacote/plugin externo, não ao binário principal.**
> **Nenhuma movimentação de código/dados nem nova dependência é autorizada por este documento. É apenas plano e documentação.**

> **Clarificação de produto posterior nesta sessão:** apenas **Pattern Maker e Tools** ficam como ferramentas criativas próprias. D-Core/F-Core e restantes módulos deixam de ser superfícies autónomas. O catálogo de padrões e o alinhamento de pratos com kick/baixo são preservados; os samples antigos distribuídos saem. Ver [direcção e plano](DIRECCAO-CORE-PATTERN-MAKER-TOOLS.md). Esta clarificação resolve a preservação do catálogo sem ratificar a folha D58–D66 do coordenador.

## 1. Autoridade e honestidade de estado

Esta fronteira regista a instrução explícita do dono de **17-11-2026**. As decisões **D-AI-fora, A09/D-DEFER1 e D47** são aqui referenciadas tal como foram comunicadas pelo dono; não foi encontrado um ledger dessas decisões neste snapshot do qwen-core e não se inventa a sua ratificação histórica.

Segundo a instrução do dono, a **D47 invalida referências que apresentem o painel/memória da PR #4 como uma decisão aprovada**. Existência de código, merge, comentário, relatório, branch antiga ou PR não constitui autorização. A PR #4 não foi re-auditada nesta tarefa.

Ordem para esta matéria: instrução actual do dono → esta fronteira → descrições históricas. Os relatórios anteriores continuam a valer como registo do que observaram/testaram; não podem autorizar arquitectura contrária à fronteira.

**Atenção: o estado exigido ainda não coincide com o código de qwen-core.** Não usar a frase “AI fora” como prova de isolamento já entregue.

| Referência | Estado e evidência |
| --- | --- |
| qwen-core auditado | `599d558ea8c3bd281a7b0be1d4530ca4c6550dba`, main consultada em 17-11-2026 |
| AICORE consultado, apenas leitura dirigida | `500bec884404ed4745c3cef1e92e4aa4360ee578`; não é auditoria integral do projecto irmão |
| Dois audits enviados pelo dono | Relatório e resumo de 17-11-2026, posteriores ao estudo UI/UX; relatam 15 testes / 113 casos / 1456 verificações em Linux |
| Execução nesta tarefa | Leitura estática e validação documental; nenhum build, runtime Windows ou teste do produto corrido |
| CORE_BUILD_AI=OFF por defeito | **Requisito do dono; não encontrado como opção no CMake raiz nem na configuração desktop/control examinada** |
| Painel dormente/offline | **Requisito; não confirmado:** painel/badge estão ligados no código; aiControl tem default true na saída real |
| ai_boundary_static | **Gate exigido; não registado em tests/CMakeLists.txt deste snapshot** |
| Core 100% sem pacote AI | **Critério de aceitação ainda por demonstrar** com pacote ausente e Windows P0 |

“Confirmado no código” não significa “comprovado por teste”. “Apenas reportado” identifica resultados dos audits fornecidos. “Não verificado” identifica o que exige execução, instalação ou dados não disponíveis.

## 2. Finalidades

### Core

DAW desktop leve, fiável e fácil de usar para música electrónica underground, em particular psycore, slambient e hitech. Windows 11 x64 é P0; uso privado, local-only, sem subscrição.

O som criativo vem de plugins externos e áudio importado/gravado. VST3 é o suporte identificado no snapshot auditado; a direcção do produto é extensível a outros tipos/formatos, cuja compatibilidade terá de ser verificada. Kick Design e Cymbal Design são explicitamente retirados da oferta própria; os painéis de Tools ainda serão explorados, com acesso directo ao editor de cada plugin da chain (PROD-08…PROD-10 em [direcção do produto](DIRECCAO-CORE-PATTERN-MAKER-TOOLS.md)). O Core garante transporte, precisão temporal, comportamento real-time previsível, baixa latência, CPU/RAM eficientes, render consistente, hosting, automação, routing, persistência e recuperação. Gain, pan, metering, resampling e protecção de saída são DSP infraestrutural permitido.

Pattern Maker e Tools escrevem dados musicais e orquestram plugins; não constituem instrumentos/efeitos próprios. KBB/rolls, alinhamento, build-ups e operações úteis por papéis podem sobreviver dentro dessas duas ferramentas, sem manter D-Core/F-Core ou outros módulos autónomos. Exportar pistas/stems do projecto é uma operação do DAW; **separar uma mistura em stems por AI** é trabalho do AICORE.

Prioridade: integridade e fiabilidade > real-time > performance > compatibilidade de plugins > clareza de workflow > quantidade de funcionalidades. Poucos cliques, profundidade sem confusão. **A AI não é dependência do motor nem da experiência principal.**

### AICORE

Projecto irmão para análise de mistura, sound design assistido, separação de stems, treino, conhecimento, experiências e memória de longo prazo. Trabalho pesado/offline fora do caminho de áudio do Core. Uma falha, reinício, alteração de modelo ou treino de horas não pode parar a música.

Separação de processos não prova ausência de contenção de CPU/RAM/GPU/disco. A independência sob carga tem de ser medida; limites e prioridade do laboratório serão política do lado externo, sem espera síncrona do audio callback.

### Futuro pacote/plugin de AI do Core

Ainda diferido. Possui modelo, configuração/chaves, chat, base de conhecimento, memória de receitas SQLite, export JSONL e ponte para o laboratório. Pode ser distribuído a partir do projecto irmão, mas a identidade do pacote e os caminhos finais exigem decisão; **AICORE laboratório e pacote opcional não são automaticamente o mesmo processo**.

“Plugin externo” aqui não autoriza uma DLL de AI dentro de Core.exe nem processamento AI no callback de um VST3. A forma de hosting deve preservar processo separado e ausência de dependência no áudio.

## 3. Fronteira física obrigatória

1. O Core conserva o catálogo explícito de acções permitidas, validação do contrato, validação da proposta inteira, aplicação transacional e um Undo.
2. O pacote externo interpreta linguagem, escolhe modelos/receitas, guarda memória/chat/chaves e conversa com o AICORE.
3. O fio futuro de controlo é **pipe local**, sem sockets, HTTP, WebSocket, servidor de rede ou fallback remoto. “localhost” continua a ser socket e não satisfaz esta regra.
4. O pacote só recebe capacidades explícitas de criação musical: batidas, estrutura e alteração de partes. Análise, sound design, separação de stems e treino **não são expostos**, mesmo que um cliente os tente invocar directamente.
5. Não há includes, libs, SQLite ou modelos cruzados entre os dois lados. O contrato versionado é mensagem/dados; nenhum lado precisa de headers internos ou checkout do outro.
6. A ponte específica AICORE, descoberta de addon, modelos, heartbeat de chat, memória e lançador não pertencem ao binário principal.
7. O receptor mínimo necessário para verificar/aplicar acções no host não é a bridge AICORE. A topologia e o local do endpoint de pipes ainda precisam de decisão antes de código; não usar esta distinção para reintroduzir um conector genérico dentro do Core.
8. O motor não espera pela AI. Propostas entram fora do audio callback, são validadas contra uma revisão identificada do documento e só produzem estado aprovado pelo host.
9. AI desactivada significa ausência efectiva de activação/polling/lançamento, não apenas esconder um botão.
10. Qualquer mudança de **dependência, processo, persistência ou âmbito** é gatilho de arquitectura e exige decisão numerada explícita antes da implementação. Migração de dados é não-destrutiva e versionada.

## 4. O que vive onde — matriz ficheiro a ficheiro

Destino é responsabilidade normativa; “extrair” é plano condicionado, não acção autorizada. Não mover ficheiros mistos inteiros por conveniência.

### 4.1 Core: ficheiros de integração e propriedade

| Ficheiro actual | Confirmado no código | Destino / tratamento planeado |
| --- | --- | --- |
| [CMakeLists.txt:42](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/CMakeLists.txt#L42) | Adiciona app/control sem opção AI | Core; futura opção OFF e separação de targets mediante decisão |
| [app/control/CMakeLists.txt:4](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/control/CMakeLists.txt#L4) | core_control STATIC contém calls.cpp e bridge.cpp | Core retém contrato/validação; retirar bridge deste target só após decisão e gates |
| [app/control/src/calls.hpp:5](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/control/src/calls.hpp#L5) | Tipos, catálogo, validação/aplicação; comentários históricos de AI e contrato ausente | Core: contrato de acções restrito; corrigir documentação de código em futura tarefa autorizada |
| [app/control/src/calls.cpp:237](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/control/src/calls.cpp#L237) | Catálogo real inclui stutter, voicestutter, mix e open | Core: verificar cada acção contra allowlist musical; não herdar permissões dos comentários antigos |
| [app/control/src/bridge.hpp:1](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/control/src/bridge.hpp#L1) | Contrato de bridge por ficheiros, explícito “no pipe” | Pacote externo para bridge AICORE; protocolo do receptor mínimo é decisão separada |
| [app/control/src/bridge.cpp:83](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/control/src/bridge.cpp#L83) | Estado/inbox/outbox, heartbeat, discovery e manifest de addon | Responsabilidade externa; adaptar transporte por plano aprovado, sem copiar cegamente |
| [app/desktop/CMakeLists.txt:19](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/desktop/CMakeLists.txt#L19) | Compila ProposalsPanel e liga core_control | Core: build principal sem painel/bridge AI no estado exigido |
| [app/desktop/src/MainComponent.h:540](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/desktop/src/MainComponent.h#L540) | Guarda inbox, estado AICORE, processo e flag AI | Separar membros de integração dos dados do DAW; não mover MainComponent inteiro |
| [app/desktop/src/MainComponent.cpp:2057](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/desktop/src/MainComponent.cpp#L2057) | Cria badge; default aiControl=true em :2069 | UI AI externa/diferida; manter UI principal funcional sem pacote |
| [app/desktop/src/MainComponent.cpp:3527](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/desktop/src/MainComponent.cpp#L3527) | Pasta Core/AI, polling, publicação e propostas | Adaptador/bridge sai para pacote; validação/commit/Undo ficam no Core |
| [app/desktop/src/MainComponent.cpp:3627](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/desktop/src/MainComponent.cpp#L3627) | Valida, ensaia em cópia e aplica proposta | Core conserva garantia transacional; mudar de filtrar parcialmente para recusar proposta inteira |
| [app/desktop/src/MainComponent.cpp:3736](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/desktop/src/MainComponent.cpp#L3736) | Procura addon em settings/pastas e inicia ChildProcess | Pacote externo; Core não deve gerir laboratório/modelo |
| [app/desktop/src/MainComponent.cpp:3807](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/desktop/src/MainComponent.cpp#L3807) | Define pasta de training clips | Configuração orientada a treino é candidata ao pacote; export musical genérico continua Core |
| [app/desktop/src/ProposalsPanel.h](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/desktop/src/ProposalsPanel.h) | Declara superfície de propostas | Externa/diferida; sem autorização para acrescentar UI no Core |
| [app/desktop/src/ProposalsPanel.cpp:17](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/desktop/src/ProposalsPanel.cpp#L17) | Apply/Dismiss, warnings e “left out” | Painel exterior; recusa do host continua obrigatória e independente do painel |
| [app/desktop/src/Settings.cpp:716](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/desktop/src/Settings.cpp#L716) | Configuração de Training clips | Classificar/migrar configuração sem apagar pasta ou dados |
| [app/domain/src/history.cpp:38](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/domain/src/history.cpp#L38) | Histórico/commit/Undo | Fica Core; nunca mover integridade documental para AI |
| [app/domain/src/edit.cpp:1103](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/domain/src/edit.cpp#L1103) | Tabelas com proveniência de conhecimento AICORE | Decisão de curadoria; não transferir edit.cpp nem apagar compatibilidade |
| [app/domain/src/role_tools.cpp](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/domain/src/role_tools.cpp) | Implementação das tools por papéis | Fica Core enquanto orquestração musical determinística; não é inferência AI |
| [app/domain/src/sample_analysis.cpp](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/domain/src/sample_analysis.cpp) | Análise de samples no domínio | Classificar por operação: metadados/infraestrutura do browser vs análise musical pesada; não mover só pelo nome “analysis” |
| [app/engine-client/src/engine_client.cpp:65](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/engine-client/src/engine_client.cpp#L65) | Pipes entre app e core-engine | Fica Core; **não é** prova de pipe de AI nem autorização para expor protocolo do motor à AI |
| [app/engine-client/src/engine_protocol.hpp:1](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/engine-client/src/engine_protocol.hpp#L1) | Parser das respostas do motor | Fica Core; isolamento e validação do motor |
| [app/installer/CMakeLists.txt:19](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/installer/CMakeLists.txt#L19) | Lista do pacote Core: executáveis e recursos, sem pacote AICORE | Fica Core; manter independência e testar instalação limpa |
| [app/installer/Setup.cpp](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/app/installer/Setup.cpp) | Instalação/manifests | Core; preservar dados do utilizador e não incluir migração AI silenciosa |
| [tests/calls_tests.cpp:123](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/tests/calls_tests.cpp#L123) | Testes actuais de calls incluem filtragem de chamadas inválidas | Core; futura suite precisa testar recusa integral, não confundir filtragem com atomicidade |
| [tests/CMakeLists.txt](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/tests/CMakeLists.txt) | Testes merge/desktop; ai_boundary_static não registado | Core: futuros gates negativos e de pacote ausente |
| [ci/gate.sh](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/ci/gate.sh) | Build Linux sem desktop e ctest merge | Core; não prova Core.exe isolado |
| [ci/gate.ps1](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/ci/gate.ps1) | Gate Windows com modo desktop | Core; acrescentar futura matriz AI OFF/pacote ausente mediante decisão |
| docs/contracts/core-calls.md | Referenciado pelos headers, ausente da árvore | Documento futuro do contrato publicado pelo Core; não inventar contrato já implementado |

Os restantes ficheiros do motor/hosting e documento continuam no Core. Não se propõe mover subsistemas de áudio para AICORE.

### 4.2 AICORE: responsabilidades externas verificadas por leitura dirigida

| Ficheiro existente | O que foi observado | Destino normativo |
| --- | --- | --- |
| [AICORE/addon.json](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/addon.json) | Manifest com programa/argumentos para abrir a app | Exterior; compatibilidade legada, não mecanismo futuro obrigatório do Core |
| [AICORE/apps/studio/src/aicore_studio/addon.py:19](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/apps/studio/src/aicore_studio/addon.py#L19) | Pasta de integração, status e comandos por ficheiros | Adaptador externo; migração coordenada com bridge legado |
| [AICORE/apps/studio/src/aicore_studio/assistant_gui.py:81](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/apps/studio/src/aicore_studio/assistant_gui.py#L81) | Chat, histórico em memória e configuração do modelo | AICORE/pacote, nunca Core.exe; persistência de chats instalada não inventariada |
| [AICORE/apps/studio/src/aicore_studio/assistant_prompt.py:15](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/apps/studio/src/aicore_studio/assistant_prompt.py#L15) | Instruções do assistente | Exterior; não transportar prompts para catálogo do Core |
| [AICORE/apps/studio/src/aicore_studio/connection.py:26](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/apps/studio/src/aicore_studio/connection.py#L26) | Aceita HTTPS/HTTP local; credenciais com funções próprias | Exterior; percurso futuro Core local-only não pode herdar upstream remoto |
| [AICORE/apps/studio/src/aicore_studio/model_setup.py](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/apps/studio/src/aicore_studio/model_setup.py) | Configuração do modelo | Exterior; não dependência de build/arranque do Core |
| [AICORE/services/memory/src/aicore_memory/core_bridge.py:308](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/services/memory/src/aicore_memory/core_bridge.py#L308) | Servidor HTTP e encaminhamento /chat/completions | Legado externo; **não compatível com o fio pipes-only** da futura integração |
| [AICORE/services/memory/src/aicore_memory/store.py:167](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/services/memory/src/aicore_memory/store.py#L167) | SQLite, migrações e store | Memória AICORE; não partilhar DB/headers/lib com Core |
| [AICORE/services/memory/src/aicore_memory/package.py:49](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/services/memory/src/aicore_memory/package.py#L49) | Export/import de pacotes; events/items/relations/policies JSONL | Exterior; preservar IDs, políticas e versões |
| [AICORE/apps/studio/src/aicore_studio/audio_tasks.py:212](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/apps/studio/src/aicore_studio/audio_tasks.py#L212) | Jobs de processamento áudio e subprocessos | Laboratório; não expor pelo catálogo musical do Core |
| [AICORE/apps/studio/src/aicore_studio/training_worker.py:42](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/apps/studio/src/aicore_studio/training_worker.py#L42) | Processo de treino e gestão de lifecycle | Laboratório, sem espera no host |
| [AICORE/docs/knowledge/README.md](https://github.com/dmo-app/AICORE/blob/500bec884404ed4745c3cef1e92e4aa4360ee578/docs/knowledge/README.md) | Base documental de conhecimento | AICORE; origem de preset estático não cria por si só dependência em runtime |

Não foi encontrado/inventariado um ficheiro concreto de **memória de receitas do futuro pacote**. A atribuição de SQLite/receitas ao pacote é requisito, não afirmação de que esse pacote já existe. Bases instaladas, chaves reais e chats pessoais não foram procurados nem lidos. A lista acima cobre as travessias relevantes observadas, não todos os módulos do laboratório.

## 5. Inventário das travessias actuais

| ID | Travessia actual | Evidência | Desvio / acção planeada |
| --- | --- | --- | --- |
| X01 | Compilação: CoreApp → core_control → bridge.cpp | CMake root/control/desktop | Contrato e bridge misturados; separação física pendente |
| X02 | Core → Core/AI/core-state.json | MainComponent :3572–3578; bridge writeState | Publica catálogo, resumo da canção e caminho indexFile; substituir por mensagens mínimas/versionadas após decisão |
| X03 | AICORE → inbox/*.json → Core | MainComponent :3582–3588; bridge readInbox | Polling de ficheiros, não pipes |
| X04 | Core → outbox/*.json; remoção de inbox | bridge finishProposal | Mensagens incluem proposta; isso é persistência de integração, não base de receitas. Migrar sem replay/duplicação |
| X05 | aicore-status.json → badge/modelo | MainComponent :3563–3568 | UI host conhece estado do chat/modelo |
| X06 | Core → aicore-command.json / lançamento addon | MainComponent :3751–3776 | Host gere abertura do laboratório; responsabilidade externa |
| X07 | samples-added.json → rescan de Library | MainComponent :3553–3561 | Leitura ocorre antes de if (!aiAllowed_); desligar propostas não corta toda a travessia |
| X08 | Core → caminho do índice de samples | MainComponent :3573–3574 | Caminho publicado; consumidor efectivo e regras de leitura não testados. Contrato futuro deve explicitar o mínimo permitido |
| X09 | Settings → aiControl/aicoreFolder/folderTrainingClips | MainComponent :3532, :3791; Settings :719 | Preferências de integração espalhadas no host; conservar originais na migração |
| X10 | Pacote antigo → HTTP/upstream/memória | AICORE core_bridge.py/connection.py | Não reutilizar como implementação do novo fio; local-only não está provado por ausência de modelo em Core |
| X11 | Conhecimento AICORE → dados estáticos patterns/edit.cpp | Anexo A e edit.cpp:1103/:1175 | Proveniência documental, não chamada de processo; destino depende de decisão de curadoria |
| X12 | Proposta parcialmente válida → subconjunto aplicado | calls.cpp:474, :664; MainComponent :3605, :3633 | Viola “tudo-ou-nada” da proposta original, apesar do ensaio em cópia/Undo único |

**Ponto crítico:** testar apenas se uma operação composta que lança excepção deixa o documento intacto não cobre X12. Se o validador deitar fora uma chamada inválida e mantiver outra, o pedido original já foi alterado. A futura regra exige recusa integral antes de commit, sem Undo vazio e sem efeitos laterais.

O catálogo actualmente construído por buildFunctions contém **stutter, voicestutter, mix e open**. Os comentários de calls.hpp sobre sintetizadores/FX próprios não descrevem esse catálogo actual. Não há prova aqui de que todas as acções Patterns/KBB/estrutura já sejam chamáveis. Mix/open exigem classificação explícita no catálogo futuro; não manter uma função só porque já estava exposta.

## 6. Correcções de interpretação dos audits anteriores

- O audit de 17-11 diz que calls.cpp **e bridge.cpp** “têm de ficar no Core”. **Esta conclusão arquitectural é substituída pela instrução do dono:** catálogo/validação/transacção/Undo ficam; bridge AICORE sai do binário principal, por implementação futura autorizada.
- “Não há modelo/chat no binário” não prova ausência de bridge, painel, IO, arranque de processos ou dependências. Uma procura textual também não é inspecção de um executável.
- “AI fora por agora” e CORE_BUILD_AI=OFF são o estado exigido; o snapshot consultado ainda contém integração activa no código e não apresenta o gate nomeado.
- O audit fala em **45 patterns com origem AICORE**. A leitura individual confirmou **45 JSON no total: 39 com source AICORE e 6 DnB com outras fontes; todos com status hypothesis**. Ver anexo A. Não apagar 45 ficheiros com base na contagem antiga.
- Proveniência AICORE não equivale a modelo, memória activa ou dependência cruzada. O dono decide se dados musicais estáticos, curados e versionados são conteúdo do Core ou catálogo externo.
- Os 15 testes/113 casos/1456 verificações são resultados **reportados pelo outro audit**, não reexecutados aqui; não demonstram ai_boundary_static nem execução Windows sem pacote.
- O estudo UI/UX anterior era de outro snapshot. A sua referência a ProposalsPanel como integração opcional não autoriza mantê-lo no binário ou acrescentar UI AI.

## 7. Plano de migração sem perda — só após decisões

### M0 — fixar contrato e inventário

Registar decisão do dono para dependências/processos/persistência/âmbito, fechar allowlist e endpoint mínimo. Criar docs/contracts/core-calls.md numa tarefa documental própria com versões, requests/responses, rejeições, limites, revisão do projecto e tratamento de repetição. Não chamar ao ficheiro da bridge actual um protocolo de pipes.

Inventariar a instalação real com o dono: pasta Core/AI, inbox/outbox, settings, receitas/SQLite, chats, exports JSONL, configurações e versões dos pacotes. Inventário só de metadados/hashes/contagens; não copiar segredos para logs, git ou relatórios.

### M1 — snapshot reversível dos dados

Parar apenas os escritores da integração, sem parar o DAW por necessidade da AI. Fazer backup consistente de SQLite pela API de backup ou mecanismo equivalente; não copiar apenas o ficheiro .db ignorando WAL. Guardar manifest de versão, hashes, contagens, IDs, estado de aplicação de propostas e origem.

Chaves mantêm protecção e contexto do utilizador: transferência explícita com mecanismo seguro do sistema, ou reconfiguração se não forem portáveis. Nunca exportar chaves em JSONL de treino. Não assumir que é preciso mover uma memória que já está no AICORE.

### M2 — importar para área nova e validar

Usar caminhos/DB de destino separados e schema versionado. Importação idempotente, com IDs estáveis e detecção de conflitos; preservar autoria, timestamps, políticas, revogações, receitas e referências de assets. Chats preservam mensagens visíveis; não inventar histórico ou raciocínio interno.

Inbox/outbox antigos são arquivados como legado. Não aplicar automaticamente propostas antigas; uma proposta pendente exige revisão actual do projecto e decisão do utilizador. Registos “applied” nunca se tornam comandos a repetir.

Comparar contagens, hashes dos payloads preservados e referências. Produzir relatório de migração sem segredos. Fonte continua intacta; falha não activa destino parcial.

### M3 — separação de build e processos

Após aprovação: separar catálogo/validador de bridge; desactivar AI por defeito de forma física; remover do build principal painel/lançador/polling de AICORE, conservando dados legados. Não levar history/document/edit para o pacote. Não criar dependência de SQLite/modelo no Core.

Futuro receptor de pipes fora do audio callback, com mensagens limitadas e sem espera síncrona na música. O transporte de controlo não usa o pipe privado do motor como atalho.

### M4 — cortar ligação antiga sem dupla aplicação

Activar transporte novo só após gates. Um único escritor e um único transporte por sessão; protocolo com request ID, revisão esperada e resultado inequívoco. Definir tratamento de retry/queda após commit para impedir duplicação, sem transformar o Core numa memória de chat/receitas.

Rollback restaura pacote/configuração anterior e dados preservados, sem desfazer projectos musicais criados depois do corte. Nenhuma limpeza automática de backups. Eliminação posterior é outra decisão numerada.

### M5 — recursos musicais e compatibilidade

A clarificação PROD-03 determina preservar o catálogo de padrões no Core: manter conteúdo, schema, proveniência e testes musicais, sem ler docs/knowledge em runtime. Classificar separadamente as tabelas antigas e dependências de samples/DSP descontinuado; não preservar um sintetizador próprio apenas porque uma tabela vinha do catálogo. Não apagar origem para esconder a ligação.

Em todas as milestones: Core compila, abre, toca, guarda, reabre e exporta sem pacote de AI. A milestone de documentação não afirma esses gates verdes.

## 8. Testes que terão de provar a fronteira

Esta é uma **especificação de testes**, não testes criados ou executados. Os nomes novos abaixo são propostas; ai_boundary_static é o nome exigido pelo dono.

| Gate/caso | Prova e oráculo | Estado actual observado |
| --- | --- | --- |
| ai_boundary_static | Em build OFF: inspeccionar grafo de targets, compile_commands/link map e pacote; bridge AICORE, ProposalsPanel, SQLite/modelos/headers externos não entram; include/dependency regressions falham | Não registado; root/control/desktop contêm ligações contrárias |
| Configuração OFF efectiva | Opção existe, default OFF e perfil limpo não compila dependências AI; detectar flag desconhecida/ignorada | CORE_BUILD_AI não encontrado na configuração examinada |
| Core sem pacote (Windows P0) | Instalação limpa sem AICORE, Python, modelo ou DB AI: abrir/tocar/editar/guardar/reabrir/exportar; nenhuma pasta/processo AI criado nem polling necessário | Não executado; self-test actual força aiAllowed=false em null output, insuficiente para provar percurso real |
| Catálogo físico | Comparar IDs expostos com allowlist aprovada; invocar directamente análise/sound-design/stems-separation/train/shell/load-model/DB e confirmar rejeição, mesmo sem UI | Catálogo actual limitado, mas não qualificado contra lista futura |
| Proposta tudo-ou-nada | Chamada válida + inválida, argumento inválido, falha tardia, alvo inexistente: documento/estado/Undo/ficheiros iguais ao anterior | Código actual filtra partes e pode aplicar restantes |
| Um Undo | Proposta inteira válida altera vários objectos numa transacção; undo/redo restaura resultado e estado de plugins quando aplicável | Ensaio em cópia e etiqueta única no código; runtime não testado |
| Revisão e identidade | Proposta atrasada de projecto anterior/revisão antiga é recusada; IDs não resolvem por nome ambíguo | Testes de calls existentes não provam todas estas condições |
| Pipe inválido | JSON malformado, tamanho excessivo, frame parcial, versão incompatível, flood, cliente morto/lento: recusa limitada, sem bloqueio da UI/áudio | Transporte AI por pipes ainda não identificado |
| Queda após commit/retry | Matar cliente após aplicação antes de receber resultado; reenviar ID: nenhuma segunda aplicação/Undo duplicado | Não testado |
| Sem rede | Instrumentar socket/connect/listen/DNS no Core e no pacote da integração; zero sockets, incluindo loopback, no percurso aprovado | Não executado; bridge HTTP externa existente não serve como prova |
| Crash/restart/carga AI | Matar/reiniciar pacote e saturar laboratório enquanto toca/grava/exporta; medir continuidade, underruns e latência contra baseline igual | Não executado; separar crash isolation de contenção de recursos |
| Persistência/segredos | Core não abre SQLite/chat/receitas/chaves; pacote ausente não impede projecto; logs sem segredos | Store externo identificado; instalação real não inventariada |
| Migração e rollback | IDs/contagens/hashes/políticas/revogações conservados; interrupção em cada fase; original intacto; reexecução idempotente | Não executado; plano apenas |
| Recursos estáticos | Abrir projectos/templates após migração de patterns; nenhuma leitura de AICORE necessária | Proveniência lida; gate não executado |
| Empacotamento/instalador | Manifest Core independente; desinstalação/upgrade não remove dados AI ou do utilizador fora de manifest | Lista de pacote observada; Windows não executado |

Guardar SHA/build, versões, plataforma, configuração, logs, casos e resultados reais. Linux sem JUCE não substitui Windows. Não marcar gate verde com zero testes nem apenas com grep. Um teste estático é necessário mas insuficiente para independência em runtime.

## 9. Questões bloqueantes e recomendação

Os identificadores **P-F01…P-F06 são propostas de decisão**, não decisões aprovadas nem substitutos do ledger do dono. A documentação pode ser publicada agora; estas questões bloqueiam implementação.

| Proposta | Questão / alternativas | Consequência | Recomendação |
| --- | --- | --- | --- |
| P-F01 | Qual o pacote/processo opcional e o endpoint mínimo que fica do lado Core? Processo separado ou outro hosting que assegure isolamento? | Define ownership, IPC e dependências; plugin in-process quebra a finalidade | Processo separado; bridge AICORE integralmente externa; receptor mínimo musical no host, se aprovado |
| P-F02 | Qual allowlist exacta? Manter mix/open? Que operações de estrutura entram? | “Criação musical” sem enumeração deixa crescer o âmbito | IDs enumerados e negativos testados; nenhuma análise/sound design/stems/treino, nenhum shell genérico |
| P-F03 | Quais dados de receitas/chats/chaves existem nas instalações a migrar? | Sem inventário não há garantia de perda zero | Inventário versionado e backup consistente antes de qualquer remoção; não criar migração fictícia |
| P-F04 | Catálogo de padrões: preservação decidida na clarificação PROD-03. Tabelas antigas: classificar o que serve Tools e o que pertence a DSP descontinuado | Preservar padrões sem manter instrumentos/efeitos próprios | Manter os 45 JSON e proveniência; decidir destino das tabelas por função, não apagar edit.cpp |
| P-F05 | Como impedir reaplicação após falha sem guardar memória AI no host? | É problema de integridade do DAW, não receita/chat | Request ID + revisão e recibo mínimo com retenção explicitamente aprovada; sem histórico conversacional |
| P-F06 | Aprovar uma tarefa posterior para fechar OFF/gates/transportes e rever perfis antigos do AICORE? | Código actual e documentos antigos não cumprem a nova regra | Corrigir OFF e prova sem pacote primeiro; não reactivar AI nem introduzir modelo agora |

**Recomendação final:** tornar esta fronteira referência visível do repo, tratar a bridge/painel actuais como dívida a resolver e não como excepção autorizada, e executar primeiro uma futura fase de isolamento com testes negativos. Só depois de decisão numerada, contrato fechado e gates verdes pode regressar a AI. Não mover nem apagar código ou dados nesta tarefa.

## Anexo A — inventário individual dos patterns

Leitura dos 45 ficheiros JSON no snapshot Core acima. Todos têm status `hypothesis`. A clarificação PROD-03 manda preservar o catálogo no Core; nenhum foi movido, apagado ou promovido a conhecimento validado.

| Ficheiro | Proveniência declarada | Destino |
| --- | --- | --- |
| [patterns/bass/kb-b-syncopated.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kb-b-syncopated.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/kbb-spaced.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kbb-spaced.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/kbbb-accent-wave.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kbbb-accent-wave.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/kbbb-anchor.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kbbb-anchor.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/kbbb-breath.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kbbb-breath.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/kbbb-eight-bar-evolution.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kbbb-eight-bar-evolution.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/kbbb-fifth-turn.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kbbb-fifth-turn.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/kbbb-gate-wave.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kbbb-gate-wave.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/kbbb-microbounce.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kbbb-microbounce.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/kbbb-octave-answer.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kbbb-octave-answer.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/kbbb-two-bar-answer.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/kbbb-two-bar-answer.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/bass/triplet-contrast.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/bass/triplet-contrast.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/break/dnb-32nd-bursts.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/break/dnb-32nd-bursts.json) | MusicRadar/FaderPro + pedido do dono (texto source) | Core — preservar catálogo (PROD-03) |
| [patterns/break/dnb-chopped-4.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/break/dnb-chopped-4.json) | MusicRadar/FaderPro + pedido do dono (texto source) | Core — preservar catálogo (PROD-03) |
| [patterns/break/dnb-double-kicks.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/break/dnb-double-kicks.json) | MusicRadar/FaderPro + pedido do dono (texto source) | Core — preservar catálogo (PROD-03) |
| [patterns/break/dnb-halftime.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/break/dnb-halftime.json) | MusicRadar/FaderPro + pedido do dono (texto source) | Core — preservar catálogo (PROD-03) |
| [patterns/break/dnb-neuro.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/break/dnb-neuro.json) | MusicRadar/FaderPro + pedido do dono (texto source) | Core — preservar catálogo (PROD-03) |
| [patterns/break/dnb-two-step.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/break/dnb-two-step.json) | MusicRadar/FaderPro + pedido do dono (texto source) | Core — preservar catálogo (PROD-03) |
| [patterns/break/hitech-hats-thin-out.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/break/hitech-hats-thin-out.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/break/hitech-offbeat-zaps.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/break/hitech-offbeat-zaps.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/build/snare-roll-2.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/build/snare-roll-2.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/build/snare-roll-4-gap.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/build/snare-roll-4-gap.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/build/snare-roll-4.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/build/snare-roll-4.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/build/snare-roll-8.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/build/snare-roll-8.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/build/snare-roll-gradual-4.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/build/snare-roll-gradual-4.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/build/tom-riser-2.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/build/tom-riser-2.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/cymbals/crash-every-4.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/cymbals/crash-every-4.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/cymbals/crash-every-8.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/cymbals/crash-every-8.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/cymbals/ride-offbeat.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/cymbals/ride-offbeat.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/cymbals/ride-quarters.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/cymbals/ride-quarters.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/drop/crash-kick-hats.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/drop/crash-kick-hats.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/drop/hitech-slam.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/drop/hitech-slam.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/drums/kick-four-last-off.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/drums/kick-four-last-off.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/drums/kick-four.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/drums/kick-four.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/hats/eighths.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/hats/eighths.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/hats/hitech-gallop.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/hats/hitech-gallop.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/hats/offbeat-open.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/hats/offbeat-open.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/hats/sixteenths-accent.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/hats/sixteenths-accent.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/lead/acid-line.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/lead/acid-line.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/lead/arp-minor-down.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/lead/arp-minor-down.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/lead/arp-minor-up-down.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/lead/arp-minor-up-down.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/lead/arp-minor-up.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/lead/arp-minor-up.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/perc/clap-two-four.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/perc/clap-two-four.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/perc/snare-sparse.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/perc/snare-sparse.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
| [patterns/perc/zap-answer.json](https://github.com/dmo-app/qwen-core/blob/599d558ea8c3bd281a7b0be1d4530ca4c6550dba/patterns/perc/zap-answer.json) | AICORE docs/knowledge | Core — preservar catálogo (PROD-03) |
