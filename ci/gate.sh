#!/usr/bin/env bash
# Gate do Core em Linux/WSL (motor + dominio, sem a app JUCE): configura, compila e corre os testes "merge".
#   ci/gate.sh [build-dir]
set -euo pipefail
src="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-$src/../core-build-gate}"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCORE_BUILD_APP=OFF -DCORE_BUILD_TESTS=ON
cmake --build "$build" --parallel "$(nproc)"
# --no-tests=error (audit 30): sem ele, uma cache com CORE_BUILD_TESTS=OFF fazia o gate dizer VERDE sem correr
# testes nenhum ("No tests were found!!!" saía com código 0). Medido 2026-11-17: rc=0 sem flag, rc=8 com flag.
ctest --test-dir "$build" -L merge --output-on-failure --timeout 600 --no-tests=error
