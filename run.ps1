#!/usr/bin/env pwsh
# Build (if needed) and run the simulator. PowerShell 7 on Windows, Linux or macOS (or Windows PowerShell 5.1).
#   ./run.ps1 --module ravenmoor --role player --hero elara     terminal client
#   ./run.ps1 -Server --port 8088                               HTTP server
#   ./run.ps1 --module meridian -i -Mature                      adult content (passes --mature): the model narrates and
#                                                               reads plain English; needs a model server (DND_LLM_URL),
#                                                               which web.ps1 -Mature starts for you
#   ./run.ps1 -Rebuild                                          force a clean configure
param(
    [switch]$Server,
    [switch]$Rebuild,
    [switch]$Mature,
    [Alias('i')] [switch]$Interactive,   # -i on its own would be ambiguous with PowerShell's -InformationAction
    [Parameter(ValueFromRemainingArguments = $true)] [string[]]$AppArgs
)
$ErrorActionPreference = 'Stop'
. (Join-Path (Join-Path $PSScriptRoot 'scripts') 'common.ps1')
$src   = PathOf $PSScriptRoot 'sim'
$build = PathOf $src 'build'

if ($Rebuild -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
$rc = Invoke-Configure $src
if ($rc) { exit $rc }
cmake --build $build
if ($LASTEXITCODE) { exit $LASTEXITCODE }

$exe = PathOf $build (Exe $(if ($Server) { 'dnd_server' } else { 'dnd_sim' }))
if ($Mature) { $AppArgs = @($AppArgs) + '--mature' }
if ($Interactive) { $AppArgs = @($AppArgs) + '--interactive' }
& $exe @AppArgs
exit $LASTEXITCODE
