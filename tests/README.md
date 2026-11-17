# Testes do Core

Desde 2026-11-16. Substituem as 11 suites antigas (removidas a 2026-11-15) com outras regras:

- **Sinais gerados** (seno, impulso, ruído com semente fixa) em `support/signals.hpp`. Nenhum WAV de teste no repo.
- **Sem goldens bit-a-bit presos a um compilador.** O determinismo testa-se dentro da mesma build (duas vezes igual,
  qualquer tamanho de bloco igual), não contra um hash guardado.
- **Um teste que não pode correr falha.** Nada é saltado em silêncio; cada falha imprime o valor medido e o esperado.
- **Um bug corrigido traz o seu teste**, que falha sem a correção.

## Níveis

| Label | O quê | Quando |
|---|---|---|
| `merge` | Suites registadas com label merge em CMakeLists.txt (fonte actual) | gate local antes de integrar mudanças |
| `desktop` | `desktop_self_test`: `Core.exe --self-test` no null device (abre a janela ~20 s) | validação desktop no Windows; não comprova dispositivo real |

## Correr

```
ctest --test-dir <build> -L merge --output-on-failure --no-tests=error # gate
ctest --test-dir <build> --output-on-failure --no-tests=error # tudo
<build>/tests/engine_tests render                          # só os casos com "render" no nome
```

Gate de um comando: `ci/gate.ps1` (Windows, PC do Diogo) ou `ci/gate.sh` (Linux/WSL). Não há workflow GitHub na árvore consultada em 17-11-2026; não anunciar CI activo. A lista de testes efectiva é tests/CMakeLists.txt.

## Onde pôr um teste novo

- Motor (projecto, render, WAV, tempo): `engine_tests.cpp`.
- Documento da app (guardar, undo, templates, projecção): `domain_tests.cpp`.
- Linha de comandos do `core-engine`: um `cli_*.cmake` e um `add_test` em `CMakeLists.txt`.
