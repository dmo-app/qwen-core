# Relatório — 2026-11-17 — separação da criação de música (core-main → core-create)

> **Estado no fim do dia (ler com os dois adenda em baixo).** Este relatório media uma extracção do **core-main**.
> Essa extracção foi **substituída**: depois da tua instrução sobre técnicas e métodos por estilo, o `core-create`
> passou a vir do **CORE-MANUAL** `8dc3a33` e está em `main` = **`eb653f8`**. O texto e os números de CC-01 a CC-10
> mantêm-se como foram medidos na altura; não foram reescritos.


## O que foi medido

- **Repositório, branch e SHA inicial/final.** Fonte: `dmo-app/core-main`, branch mais recente medido com
  `git for-each-ref --sort=-committerdate refs/remotes/origin` → `origin/main` = `52d46c039fb47751b7d0fbe287f9026b900c314c`
  (2026-11-17), à frente de `feat/painel-plugins-por-fabricante` (`a77b76b`) e `fix/fx-remover-efeitos-proprios` (`ee473c2`);
  todos os `claude/*`, `audit/*`, `test/*` e `feat/ai-core` são de 2026-11-16 ou antes. Destino:
  `dmo-app/core-create`, que estava **vazio** (API: `size: 0`, default branch `main`, criado 2026-11-17T14:08Z),
  agora `main` = `de92f909a43f78e196f12d2b6d7bbff1d0918f2d`. O qwen-core não entrou neste trabalho.
- **Âmbito e autorização.** Instrução actual do dono: *"no core-main para core-create, toda a informação sobre a
  app fica fora, só quero sobre criação de música por estilo"*, com *"pode se meter tudo no main"*. Executado como
  extracção **só de leitura**: nada foi movido, apagado ou alterado no core-main; nada foi tocado no qwen-core além
  deste relatório e do ESTADO.
- **Ambiente.** Linux, Python 3.12, git 2.x. Não JUCE, não Windows, não Core aberto.
- **Testes executados (ou "nenhum").** Nenhum teste do produto foi corrido — esta entrega não muda código.
  A verificação é de ficheiros: contagens, byte-identidade e grep de fuga de conteúdo de app.
- **Limitações.** Não medido: se a app lê alguma coisa daqui, se os padrões copiados carregam igual no Core,
  se o gerador corre fora de Linux/Python 3.12, e como soa qualquer coisa. Não há prova de runtime.

## Resultados

| ID | Problema / resultado | Evidência (SHA, ficheiro:linha ou execução) | Classificação | Estado |
| --- | --- | --- | --- | --- |
| CC-01 | Branch mais recente do core-main é `origin/main` (`52d46c0`), não um `claude/*` | `git for-each-ref --sort=-committerdate` no checkout | reportado | validado |
| CC-02 | Repo destino existia e estava vazio, `main` por omissão | API `repos/dmo-app/core-create` → `size: 0`, `default_branch: main` | reportado | validado |
| CC-03 | Estrutura por estilo entregue: 8 pastas (`estilos/<id>/`), 7 do catálogo PCore + `psytrance` | `core-create de92f90`, `INDICE.md`; catálogo em `pcore/styles.json` (7 entradas) | reportado | validado |
| CC-04 | 45 padrões copiados um por um, **45/45 byte-idênticos** ao blob do git | `padroes/*.json` no commit; verificação no gerador (`git show` cru vs ficheiro escrito) | teste | validado |
| CC-05 | 21 templates → 21 fichas de criação; de **3,2 MB** de markdown do Core ficaram **68 KB** | `estilos/*/fichas/*.md`; médias no `fontes.md` | reportado | validado |
| CC-06 | Conteúdo de app efectivamente fora das fichas | `grep -l "Efeitos (\|Automação:\|Modulação (\|LFOs da pista" estilos/*/fichas/*.md` → 0 ficheiros; as únicas tabelas são as de secções em `## Tempo e forma` (8 linhas/ficha) | teste | validado |
| CC-07 | Caminhos de amostras da máquina do dono cortados ao nome do ficheiro | `…\07 KICK A.wav` em `estilos/darkpsycore/fichas/psycore-blacktide.md:45` | reportado | corrigido |
| CC-08 | Etiqueta `psytrance` nos 45 padrões sem entrada em `pcore/styles.json` e sem template | `estilos/psytrance/estilo.md` diz explicitamente que não há entrada; nada inventado | reportado | aberto — decisão de curadoria |
| CC-09 | Fronteira app↔criação é escolha minha, não decisão numerada | Secção "Alterações" abaixo | reportado | decisão |
| CC-10 | Dados continuam a existir nos dois sítios | `patterns/`, `templates/`, `pcore/` intactos no core-main `52d46c0` (só `git show`/`ls-tree` usados) | código | validado |

## Alterações

**Documentação / dados, nenhum código de produto:**

- `dmo-app/core-create` (novo conteúdo, 92 ficheiros, 688 KB):
  - `INDICE.md` — uma linha por estilo com BPM, root, nº de templates e padrões.
  - `estilos/<id>/estilo.md` — carácter, BPM, root, humanize/chaos, align, escalas e formas preferidas por parte,
    tabela de templates (BPM, compassos, ≈minutos, secções) e pointer para os padrões.
  - `estilos/<id>/padroes.md` + `padroes.json` — os padrões que declaram o estilo (156 referências:
    darkpsycore 45, hitech 44, slambient 34, psytrance 33), com descrição, tags, fonte e estado.
  - `padroes/*.json` — cópia fiel, uma só vez por padrão; os estilos apontam para aqui em vez de duplicarem.
  - `estilos/<id>/fichas/<template>.md` — receita: andamento, duração, forma (secções e durações), ganho do master,
    e por pista instrumento/amostra, nível+pan e contagem de notas.
  - `catalogo/{escalas,partes,formas,aligns}.md` + `catalogo/pcore-estilos.json` — o que é comum a todos.
  - `fontes.md` — proveniência (39 padrões de AICORE `docs/knowledge`, 6 de DnB/MusicRadar; 21 templates `builtin`)
    e as contas da extracção.
  - `.gitattributes` com `* -text`, para o git não reescrever os bytes copiados em Windows.
  - `ferramentas/gerar.py` — a extracção, reprodutível.
- Neste repo (qwen-core): este relatório (documentação) e uma linha em `ESTADO.md`.

**O corte app↔criação, explícito para poderes mover.** Ficou dentro: tudo o que se pode fazer sem a app —
estilo, andamentos, forma, escalas, grelhas das formas, padrões (notas/ticks/velocity), que instrumento ou amostra
toca cada pista, nível e pan, quantas notas. Ficou fora: tabelas de parâmetros de plugins e dos sintes próprios
(EQ banda a banda, compressores, filtros, GLITCH, limiters), cadeias de FX, automação, modulação W-Core, LFOs,
encaminamentos para buses, caminhos absolutos de samples, lista de plugins e qualquer campo de UI do documento.
Se quiseres as tabelas de parâmetros dentro, é uma bandeira no gerador — não é uma reescrita manual.

## Verificação

```sh
git -C <core-main> for-each-ref --sort=-committerdate --format='%(committerdate:short) %(refname:short) %(objectname:short)' refs/remotes/origin
python3 - <<'PY'   # byte-identidade das cópias, fora do gerador
# para cada patterns/**/*.json em origin/main: git show | bytes == bytes do ficheiro em core-create
PY
```
- Contas escritas em `fontes.md`: `45 ficheiros em padroes/, 45/45 byte-idênticos ao blob do git`;
  `21 templates → 21 fichas`; `Etiquetas de estilo resolvidas pelas listas match do PCore: todas`;
  `Estilos com pasta: 8 (7 do catálogo + 1 só declarados nos padrões)`.
- Fugas de app nas fichas: `grep -l "Efeitos (\|Automação:\|Modulação (\|LFOs da pista" estilos/*/fichas/*.md` → **0** ficheiros.
- Tamanho: `du -sh` = 688 KB, 92 ficheiros; fichas = 68 KB contra 3,2 MB de `ficha` no Core (medido no JSON).
- Push: `git ls-remote --heads origin` → `de92f909a43f78e196f12d2b6d7bbff1d0918f2d refs/heads/main`;
  API `git/trees/de92f90?recursive=1` → 92 blobs, `truncated: false`.
- Vermelho/verde: **não aplicável** — nenhuma alteração de código; nenhum gate corrido.
- O gerador é determinístico a partir de `origin/main`; reler com outra ref dá outro resultado, e é essa ref que
  fica escrita em `README.md` e `fontes.md`.

## Pendências e passagem

1. **`psytrance`** (CC-08): está nos padrões, não está no catálogo e não tem template. Ou entra em
   `pcore/styles.json` com BPM/root/formas, ou é etiqueta a limpar. Decisão de curadoria do dono — não inventei entrada.
2. **Fonte da verdade** (CC-10): o core-create é uma *vista*, não um substituto. A app lê `patterns/`,
   `templates/` e `pcore/` do core-main. Passar qualquer um dos lados a ler daqui é mudança de arquitectura
   (D50) e precisa de decisão numerada; não fiz nada disso. A regra de hoje do AICORE (add-on fora do binário)
   aponta no mesmo sentido: se o conhecimento de criação vive fora do Core, quem o lê é o pacote, não o `Core.exe`.
3. **Fronteira do corte** (CC-09): o critério está escrito acima para ser contestável. Mexe nela e eu regenero.
4. **Risco residual**: os 45 JSON em `padroes/` são uma segunda cópia dos dados. Se forem editados aqui, o Core
   não os vê e os dois divergem silenciosamente. Recomendação: tratar `padroes/` como derivação (regenerar, não editar)
   enquanto a app não for apontada para aqui; decidir depois de (2).
5. **Não medido de propósito**: nada foi aberto nem ouvido. Um `git show`-a-`git show` não prova som.

---

## Addendum — 2026-11-17, mais tarde no mesmo dia: ainda não é JSON

O dono, ao ver a primeira versão: **"essa informação não é para entrar em json ainda"**. Correcção de direcção,
não de factos — o conteúdo pedido era este, mas publicado como ficheiros de dados não está autorizado ainda.
Registo aqui em adendum datado, sem reescrever o acima (regra 9 do procedimento).

**O que mudou em `core-create`** (`main` = `de92f90` → **`2d438c9`**):

- Fora os **50 JSON** que eu tinha publicado: 45 cópias de `patterns/*.json`, 4 manifestos `estilos/<id>/padroes.json`
  e o `catalogo/pcore-estilos.json` cru. Não apaguei nada do core-main — o que saiu foi a cópia; o commit anterior
  fica no histórico do repo se for preciso comparar.
- Uma **página por padrão** em `padroes/<papel>__<id>.md`: origem, papel, estado, fonte, root (com o nome que o
  próprio Core dá à nota), comprimento, estilos que o declaram, tags, e a tabela das notas todas
  (tick, compasso.tempo, duração, nota, velocity). **1034 notas** nos 45 padrões.
- O catálogo passa a `catalogo/estilos.md` com os campos todos do `pcore/styles.json`; a tabela dos 7 estilos
  está lá, a das partes continua em `formas.md`.
- Repo actual: **87 blobs, 0 JSON, 86 markdown, 343 KB** (medido pela API `git/trees/2d438c9?recursive=1`).

**Verificação nova**, feita à parte do gerador (reparsei as páginas escritas e comparei com o git do core-main,
nota a nota — tick, duração, nome da nota pela regra `template_sheet.cpp:31`, velocity): **45/45 páginas
conformes, 0 divergências, 0 páginas em falta**. Links locais: 389, **0 partidos**. `find -name '*.json'`: **0**.

**Erro meu, medido e corrigido neste addendum:** no commit anterior o **BPM do `INDICE.md` não vinha do catálogo**
— vinha do último template do estilo, porque a variável do laço se sobrepunha à do estilo. Ficou publicado
darkpsycore **210** (é 225), hitech **220** (é 190), slambient **250** (é 300). Os `estilo.md` estavam certos; o
índice não. Corrigido no gerador (`index_rows` vai buscar `(st or {}).get("bpm")`), verificado acima.

**Itens afectados deste relatório:**

| ID | Antes | Agora |
|---|---|---|
| CC-04 (45/45 byte-idênticos) | válido | **sem objecto** — não há cópias de bytes; em vez disso 1034/1034 notas conferidas |
| CC-05, CC-06 (fichas 3,2 MB → 68 KB, sem tabelas de app) | válido | **inalterado** |
| CC-10 (dados nos dois sítios) | duplicado de ficheiros | **transcrição**, não duplicado: nenhuma estrutura de dados publicada |
| Pendência 4 (risco de divergência silenciosa em `padroes/*.json`) | aplicável | **adiado** — volta quando decidires o formato JSON |

**O que fica.** A informação está toda legível e organizada por estilo, como pediste, e não há um único JSON.
Quando disseres o formato (nome de campo, se as notas ficam em tabela markdown ou em ficheiro próprio, se
`core-create` é fonte ou relatório), o gerador volta a emitir — ele já leu tudo isso do core-main uma vez.

## Addendum — 2026-11-17, terceiro do dia: a fonte era o CORE-MANUAL e o que tu querias era o método

A tua instrução, palavra por palavra: **"a informaçao que quero é as tecnicas, metodos, de criaçao de cada estilo,
pods meter psytrance como mais um estilo"** — e a de antes, que eu não tinha seguido à letra: *"no core-manual para
core-create, toda informaçao sobre a app fica fora, so quero sobre criaçaio de musica por estilo"*.

**O erro que aqui se corrige, dito claramente.** Eu extraí do **core-main**. Tu tinhas escrito *core-manual*, e o
CORE-MANUAL é mesmo o sítio do conhecimento: `AICORE/docs/knowledge/README.md` diz que o conhecimento sobre o Core
só existe no CORE-MANUAL (decisão tua, 2026-11-07, "a informação fica num só lado, para não haver conflitos").
As duas versões que publiquei (`de92f90`, `2d438c9`) eram **a forma dos dados** em markdown — padrões, tabelas de
notas, fichas de templates. Não eram técnicas nem métodos. Isto não é um refactor: era o repo errado.

**O que está publicado agora** — `dmo-app/core-create`, `main` = **`eb653f8`**, fonte **CORE-MANUAL `8dc3a33`**:

- **67 ficheiros**: 62 de conteúdo, 4 de topo (`README`, `INDICE`, `fontes`, `estatutos`) e
  `ferramentas/gerar-core-create.py`, que reproduz tudo a partir da revisão fixa do manual (`git show`, só leitura).
  **0 JSON** — mantém-se a tua instrução de hoje.
- **4 estilos** em `estilos/<estilo>/`, cada um com páginas por tema: carácter e tempo (com as variantes de base),
  ritmo e baixo, estrutura e arranjo, som dos instrumentos, efeitos e automação, níveis e mistura, masterização,
  faixas de referência, samples que encaixam, procedimentos, fora do estilo, subestilos, lacunas e fichas de técnica.
- **psytrance entrou como estilo a sério** (15 páginas): os dois tutoriais transcritos (145 e 140 BPM), os perfis de
  darkpsy / twilight / full-on / progressive, e as lacunas dele à vista — sem template, sem níveis aprovados, sem
  referência medida, como o manual diz.
- `geral/` com o que serve todos os estilos: regras comuns, gramática KBBB e baixo elástico, fichas comuns, perfis
  de produção por família e os capítulos de método do guia (kick e baixo, síntese, arranjo, mistura, samples,
  fluxo de trabalho, da ideia à faixa).
- Cada ficheiro traz no cabeçalho o caminho exacto da fonte no CORE-MANUAL e a revisão. **Nada foi reclassificado**:
  o `[estatuto]` e a chave viajam com a linha (`estatutos.md` conta-os: 104 `[codigo]`, 48 `[online]`,
  22 `[decisao_diogo]`, 22 `[estudo]`, 19 `[hipotese]`, 11 `[medido]`, 11 `[externo]`).

**Quanto é app, e o que foi cortado** (o gerador conta e volta a contar ao correr): 200 frases com informação da app,
41 itens de lista que eram só app, 147 ligações internas do manual, 38 fragmentos de frase cortada, 15 linhas de
tabela, 7 colunas e 3 tabelas inteiras, 7 secções (`Plugins`, `Biblioteca D:\Samples`, `Estado do motor`,
`Parâmetros reais dos plugins`, `Ferramentas MCP`, etc). Do que sobrou foi feita **auditoria**: **1 ocorrência** de
nome da app no corpo das páginas — `STUTTER` dentro do exemplo de pedido ("Ask: …") da ficha de psytrance. Não a
cortei porque a frase é informação de estrutura musical; está dito no `fontes.md`, não escondido.

**Verificação medida, fora do gerador.** Links internos: 119, **0 partidos**. `find -name '*.json'`: **0**.
Cabeçalho de proveniência: **62/62** ficheiros de conteúdo. Texto corrompido ou caracteres inesperados: **0**.
Contas de conteúdo no `fontes.md`: 374 itens de lista na parte consumida das 17 fontes; 518 linhas extraídas
(itens + parágrafos, com repetição quando a mesma fonte alimenta várias páginas).

**Não medido — não afirmo.** Se esta granulometria (página por tema, por estilo) é a que serve; se os capítulos do
guia devem viver aqui ou só no manual (deixei-os em inglês, sem traduzir, para não inventar); se darkpsy, twilight,
full-on e progressive hão-de ser pastas de estilo próprias em vez de páginas dentro de psytrance; e quem lê o
`core-create` — tu, a IA, ou o programa — porque isso decide o formato, não o conteúdo.

**Efeito nos itens deste relatório.** CC-01 a CC-10 mediram a extracção do core-main; **ficam ultrapassados como
fonte** e mantenho o texto acima sem reescrita (regra 9). Nada foi apagado: a tag **`antes-core-manual`** aponta para
`2d438c9` e os 87 blobs anteriores continuam no histórico do repo. **O core-main não foi tocado** — continua em
`52d46c039`, e só usei `git show`/`ls-tree`.


---

## Addendum — 2026-11-17 (correcção de números do addendum anterior)

Ao rever a árvore publicada antes de fechar, contei outra vez com `ls`/`wc` e estavam dois erros meus no texto acima.
Não reescrevo o que escrevi; corrijo aqui.

1. Escrevi que o psytrance tinha **15 páginas**. Medido na árvore de `eb653f8` são **12**. O motivo não é o gerador:
   a folha `estilos/psytrance.md` do CORE-MANUAL (`8dc3a33`, 156 itens de lista) **não tem** as secções
   *Efeitos por papel*, *Automação*, *Faixas de referência* e *Fora do estilo*, por isso essas páginas não existem.
   É lacuna da fonte e está registada como pendência no `ESTADO.md` e no `fontes.md` do próprio repo.
2. Escrevi "**1595 linhas / 141 KB**" sem dizer o âmbito. Esse número são as **62 páginas de conteúdo**; o repo todo
   (conteúdo + 4 páginas de topo, em markdown) é **1817 linhas / 155 KB**, e com o gerador são 190 762 bytes.

Contagem de páginas por estilo, conferida ficheiro a ficheiro: darkpsycore **13**, hitech **13**, slambient **12**,
psytrance **12**, mais `geral/` **12** = 62; topo **4** + `ferramentas/gerar-core-create.py` = **67 ficheiros, 0 JSON**.
Os demais números deste dia (cortes 200/147/41/38/15/10/7/7/3, 374 itens consumidos → 518 linhas extraídas,
auditoria final com **1** ocorrência de nome do Core) voltaram a bater com o que está publicado.

Para fechar: o `fontes.md` do repo passou a levar esta conta de páginas por estilo, por isso **a ponta de `main`
deixou de ser `eb653f8` e é agora `02921ea`** — a única diferença entre os dois é esse parágrafo de contagem.
Laço interno revalidado: 119 links locais, 0 partidos; JSON: 0.
