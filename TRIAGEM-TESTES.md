# Triagem dos testes herdados — qwen-core (2026-11-17)

> **Referência histórica do snapshot indicado abaixo.** Direcção e pendências actuais: [ESTADO](ESTADO.md) e [índice](docs/README.md). Não constitui autorização para reintroduzir funcionalidades antigas.

Pedido do Diogo: "nao sigas a regra os testes ci anteriores pq o claude fez muita merda, nao se sabe o que realmente se aplica".
Esta triagem decide o que se aplica. Medido no qwen-core@5d0b584, Linux, `cmake -DCMAKE_BUILD_TYPE=Release`, build limpo, 81/81.

## Correção primeiro
O commit baseline 5d0b584 escreveu "historia antiga nao importada: comeca aqui" — **errado**: a história completa do core-main veio como pais (f95b94d). Mantem-se: a proveniência é útil; o texto fica corrigido aqui.

## O que foi medido agora

| Suite | Casos | Verificações | Estado (Linux, hoje) | Veredicto |
|---|---|---|---|---|
| engine_tests | 14 | 47 | passa | manter — mede o motor real (sampleRate, picos, render) |
| domain_tests | 9 | 64 | passa | manter — documento, guardar/reabrir, undo, templates |
| role_tests | 19 | 201 | passa | manter — papeis + tools (SUCK/PUM/F-Core/shared bus/zona multi-pista) |
| midi_tests | 7 | 108 | passa | manter — import/export MIDI |
| bug_tests | 9 | 35 | passa | manter — regressões nomeadas, incl. FX removivel (bug 2026-11-17) |
| bitwig_tests | 2 | 25 | passa | manter — export Bitwig |
| audit_tests | 5 | 48 | passa | manter — ronda 1 de audit |
| audit2_tests | 5 | 66 | passa | manter — ronda 2 |
| hunt_tests | 11 | 612 | passa | manter — a mais densa; caçadas reais |
| round3_tests | 7 | 82 | passa | manter — ronda 3 + chaves de tradução |
| cli_* (4) | — | — | passam | manter — `--version`, render, measure, unicode |

**Total: 88 casos, 1288 verificações, 0 falhas. Nada está partido desde origem.**

## Sinais de "merda" procurados — não encontrados
- casos `TEST()` sem um único `CHECK`: **zero**;
- tautologias (`CHECK(true)`, `CHECK_EQ(x, x)`): **zero**;
- testes desativados/comentados: **zero**;
- margens afrouxadas: nenhuma sem justificação medida (o maior afrouxamento é `bpmAt ±0,05` com motivo escrito e um `1.5/2^23` medido 2026-11-16).

## O que esta suíte NÃO cobre (e fica dito, não escondido)
1. **Windows** — nada aqui valida o runtime Windows: janela, menus, plugin hosts, self-test. O `desktop_self_test` (label `desktop`) só corre em Windows com JUCE.
2. **Código de UI** — MainComponent/PluginCards/PatternsWindow não têm testes unitários; o painel por fabricante e o dialog de compassos são prova de compilação apenas.
3. **AI catalog paridade** — `calls.cpp` expõe pouco do que as tools fazem (gap conhecido, à espera de decisão).
4. O CI antigo (`.github/workflows/ci.yml`) **não foi importado** — continua em core-main; reconstruir CI no qwen-core precisa do scope `workflow` no token.

## Decisão
Nada é apagado nesta triagem. As suites ficam como rede de regressão **verificada hoje**, não como fé herdada. Qualquer teste que o próximo audit mostre que testa merda sai com número e justificação, não em silêncio.
