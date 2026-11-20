#!/usr/bin/env pwsh
# Build (if needed) and run every test: unit/integration (dnd_tests) and the HTTP end-to-end smoke tests (these need Python 3).
# PowerShell 7 on Windows, Linux or macOS (or Windows PowerShell 5.1).
#   ./test.ps1            run all
#   ./test.ps1 -Rebuild   clean configure first
param([switch]$Rebuild)
$ErrorActionPreference = 'Stop'
. (Join-Path (Join-Path $PSScriptRoot 'scripts') 'common.ps1')
$src   = PathOf $PSScriptRoot 'sim'
$build = PathOf $src 'build'

if ($Rebuild -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
$rc = Invoke-Configure $src
if ($rc) { exit $rc }
cmake --build $build
if ($LASTEXITCODE) { exit $LASTEXITCODE }

ctest --test-dir $build --output-on-failure
exit $LASTEXITCODE
