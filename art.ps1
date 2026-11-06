# Generate pictures from the game data with a local Stable Diffusion server (AUTOMATIC1111, Forge or SD.Next, started with --api).
#   .\art.ps1 -List                     show every prompt, generate nothing
#   .\art.ps1                           make whatever is missing
#   .\art.ps1 -Only characters,monsters -Force
#   .\art.ps1 -Ids thorin,zombie
#   .\art.ps1 -Url http://192.168.1.20:7860
# Output goes to sim\art\<kind>\<id>.png; the web client shows it as soon as it exists. Style: sim\art\style.json.
param(
    [string]$Url = $(if ($env:SD_URL) { $env:SD_URL } else { 'http://127.0.0.1:7860' }),
    [string[]]$Only = @(),
    [string[]]$Ids = @(),
    [switch]$Force,
    [switch]$List
)
$ErrorActionPreference = 'Stop'
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { $py = Get-Command python3 -ErrorAction SilentlyContinue }
if (-not $py) { Write-Host 'art.ps1: python is not on PATH' -ForegroundColor Red; exit 1 }
$script = Join-Path $PSScriptRoot 'sim\tools\gen_art.py'
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
