# Gate do Core antes de um merge (Windows, PC do Diogo): configura, compila e corre os testes.
#
#   powershell -ExecutionPolicy Bypass -File ci\gate.ps1                       # testes "merge" (~3-4 min com a build)
#   powershell -ExecutionPolicy Bypass -File ci\gate.ps1 -Desktop              # + o self-test da app (abre o Core ~20 s)
#   powershell -ExecutionPolicy Bypass -File ci\gate.ps1 -Build D:\Core-build\x
#
# Codigo de saida: 0 = verde, 1 = configure/build/testes falharam. O resumo fica em <Build>\gate-resumo.txt.
# Nao instala nada, nao escreve na arvore do codigo (o build vai para fora dela).
param(
  [string]$Build = "",
  [switch]$Desktop,
  [string]$Juce = "D:/Core-deps/JUCE-9.0.5",
  [int]$Jobs = 8
)
$ErrorActionPreference = "Stop"
$src = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Build) { $Build = Join-Path "D:\Core-build" ("gate-" + (Split-Path $src -Leaf)) }
New-Item -ItemType Directory -Force -Path $Build | Out-Null

# vcvars64: o VS 18 BuildTools (o VS 2022 desta maquina nao tem vcvars64.bat); senao, o que o vswhere encontrar.
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path $vswhere) {
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vs) { $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat" }
  }
}
if (-not (Test-Path $vcvars)) { Write-Error "vcvars64.bat nao encontrado"; exit 1 }

$juceArg = ""
# Audit 31: a definição vai para o .bat JÁ entre aspas — um caminho com espaços partia-se em dois argumentos.
if (Test-Path $Juce) { $juceArg = '"-DFETCHCONTENT_SOURCE_DIR_JUCE=' + $Juce + '"' }
$label = if ($Desktop) { "" } else { "-L merge" }
$log = Join-Path $Build "gate.log"
# Um .bat temporario numa pasta sem espacos: o cmd /c com caminhos com espacos ja partiu launchers antes.
$bat = Join-Path $env:TEMP "core-gate.bat"
@"
@echo off
call "$vcvars" > "$Build\vcvars.log" 2>&1 || exit /b 1
cmake -S "$src" -B "$Build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCORE_BUILD_TESTS=ON $juceArg || exit /b 1
cmake --build "$Build" --parallel $Jobs || exit /b 1
REM Audit 30: --no-tests=error fecha o verde-falso quando a cache tem CORE_BUILD_TESTS=OFF (medido em Linux: rc 0 -> 8).
ctest --test-dir "$Build" $label --output-on-failure --timeout 600 --no-tests=error || exit /b 1
"@ | Set-Content -Encoding ASCII $bat

$t0 = Get-Date
# O stderr junta-se ao stdout dentro do cmd: no PowerShell 5.1, com "Stop", um aviso do cmake no stderr redirigido
# pelo PowerShell (*>&1) virava erro fatal e o gate morria a meio do configure (medido no PC, 2026-11-16).
$ErrorActionPreference = "Continue"
cmd /c "`"$bat`" 2>&1" | Tee-Object -FilePath $log
$rc = $LASTEXITCODE
$ErrorActionPreference = "Stop"
$secs = [int]((Get-Date) - $t0).TotalSeconds
$commit = (git -C $src rev-parse --short HEAD 2>$null)
$dirty = if (git -C $src status --porcelain 2>$null) { " (com alteracoes por commitar)" } else { "" }
$verdict = if ($rc -eq 0) { "VERDE" } else { "VERMELHO" }
$tests = Select-String -Path $log -Pattern "tests passed|tests failed|FAILED|Failed" | ForEach-Object { $_.Line }
@(
  "Gate do Core: $verdict",
  "Codigo: $src @ $commit$dirty",
  "Build:  $Build",
  "Testes: $(if ($Desktop) { 'todos (merge + desktop)' } else { 'merge' })",
  "Tempo:  $secs s",
  ""
) + $tests | Set-Content -Encoding UTF8 (Join-Path $Build "gate-resumo.txt")
Get-Content (Join-Path $Build "gate-resumo.txt")
exit $(if ($rc -eq 0) { 0 } else { 1 })
