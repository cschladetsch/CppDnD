# Build (if needed) and run every test: unit/integration (dnd_tests) and the HTTP end-to-end smoke test.
#   .\test.ps1            run all
#   .\test.ps1 -Rebuild   clean configure first
param([switch]$Rebuild)
$ErrorActionPreference = 'Stop'
$src   = Join-Path $PSScriptRoot 'sim'
$build = Join-Path $src 'build'

if ($Rebuild -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    cmake -S $src -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}
cmake --build $build
if ($LASTEXITCODE) { exit $LASTEXITCODE }

ctest --test-dir $build --output-on-failure
exit $LASTEXITCODE
