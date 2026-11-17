# Relatório — 2026-11-17 — separação da criação de música (core-main → core-create)

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
3. **Fronteira do corte** (CC-09): o critério está escrito acima para ser contestável. Mouve-o e regenero.
4. **Risco residual**: os 45 JSON em `padroes/` são uma segunda cópia dos dados. Se forem editados aqui, o Core
   não os vê e os dois divergem silenciosamente. Recomendação: tratar `padroes/` como derivação (regenerar, não editar)
   enquanto a app não for apontada para aqui; decidir depois de (2).
5. **Não medido de propósito**: nada foi aberto nem ouvido. Um `git show`-a-`git show` não prova som.
