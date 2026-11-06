# Build (if needed) and run the simulator.
#   .\run.ps1 --module ravenmoor --role player --hero elara     terminal client
#   .\run.ps1 -Server --port 8088                               HTTP server
#   .\run.ps1 -Rebuild                                          force a clean configure
param(
    [switch]$Server,
    [switch]$Rebuild,
    [Parameter(ValueFromRemainingArguments = $true)] [string[]]$AppArgs
)
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

$exe = Join-Path $build ($(if ($Server) { 'dnd_server' } else { 'dnd_sim' }) + '.exe')
& $exe @AppArgs
exit $LASTEXITCODE
