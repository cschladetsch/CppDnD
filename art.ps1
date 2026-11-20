#!/usr/bin/env pwsh
# Generate pictures from the game data with a local Stable Diffusion server (AUTOMATIC1111, Forge or SD.Next, started with --api).
#   ./art.ps1 -List                     show every prompt, generate nothing
#   ./art.ps1                           make whatever is missing
#   ./art.ps1 -Only characters,monsters -Force
#   ./art.ps1 -Ids thorin,zombie
#   ./art.ps1 -Url http://192.168.1.20:7860
# Output goes to sim/art/<kind>/<id>.png; the web client shows it as soon as it exists. Style: sim/art/style.json.
# PowerShell 7 on Windows, Linux or macOS (or Windows PowerShell 5.1). Needs Python 3.
param(
    [string]$Url = $(if ($env:SD_URL) { $env:SD_URL } else { 'http://127.0.0.1:7860' }),
    [string[]]$Only = @(),
    [string[]]$Ids = @(),
    [switch]$Force,
    [switch]$List
)
$ErrorActionPreference = 'Stop'
# python3 first: on Linux and macOS "python" is often missing or Python 2; on Windows python3 may be the Store stub, which fails --version
$py = $null
foreach ($n in 'python3', 'python') {
    $c = Get-Command $n -ErrorAction SilentlyContinue
    if ($c) { $v = ''; try { $v = (& $c.Source --version 2>&1) | Out-String } catch {}; if ($v -match 'Python 3\.') { $py = $c; break } }
}
if (-not $py) { Write-Host 'art.ps1: Python 3 is not on PATH' -ForegroundColor Red; exit 1 }
$script = [IO.Path]::Combine($PSScriptRoot, 'sim', 'tools', 'gen_art.py')
$args_ = @($script, '--url', $Url)
if ($Only) { $args_ += @('--only', ($Only -join ',')) }
if ($Ids)  { $args_ += @('--ids', ($Ids -join ',')) }
if ($Force) { $args_ += '--force' }
if ($List) { $args_ += '--list' }
else {
    & $py.Source $script --url $Url --check
    if ($LASTEXITCODE) { exit 1 }
}
& $py.Source @args_
exit $LASTEXITCODE
