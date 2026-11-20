# Build the 3D assets for the web viewer (sim\web\models.html) with Blender: surface textures and character models.
#   .\assets.ps1                     make whatever is missing (textures for each world, a model for each hero)
#   .\assets.ps1 -NewTextures        add one more variant of every world's textures; the viewer lists it
#   .\assets.ps1 -Size 512           bigger textures (default 256)
#   .\assets.ps1 -RebuildCharacters  rewrite models that already exist (they are otherwise kept)
#   .\assets.ps1 -Only textures      or: characters
# Needs Blender (winget install BlenderFoundation.Blender) on PATH or in Program Files, or Python with `pip install bpy`.
# Output goes to sim\web\assets; the viewer is http://127.0.0.1:8080/models.html when the server is running.
param(
    [switch]$NewTextures,
    [switch]$RebuildCharacters,
    [int]$Size = 256,
    [string]$Only = '',
    [string]$Themes = ''
)
$ErrorActionPreference = 'Continue'
$script = Join-Path $PSScriptRoot 'sim\tools\gen_assets.py'
$a = @('--data', (Join-Path $PSScriptRoot 'sim\data'), '--out', (Join-Path $PSScriptRoot 'sim\web\assets'), '--size', $Size)
if ($NewTextures) { $a += '--new-textures' }
if ($RebuildCharacters) { $a += '--rebuild-characters' }
if ($Only) { $a += @('--only', $Only) }
if ($Themes) { $a += @('--themes', $Themes) }
$blender = Get-Command blender -ErrorAction SilentlyContinue
if ($blender) { $blender = $blender.Source }
else {
    $blender = Get-ChildItem "$env:ProgramFiles\Blender Foundation\Blender *\blender.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if ($blender) {
    & $blender -b --factory-startup -P $script -- @a
    if ($LASTEXITCODE -ne 0) { Write-Host 'assets.ps1: Blender failed' -ForegroundColor Red; exit 1 }
} else {
    $py = Get-Command python -ErrorAction SilentlyContinue
    if (-not $py) { Write-Host 'assets.ps1: Blender is not installed (winget install BlenderFoundation.Blender) and python is not on PATH' -ForegroundColor Red; exit 1 }
    & python -c 'import bpy' 2>$null
    if ($LASTEXITCODE -ne 0) { Write-Host 'assets.ps1: install Blender, or run: pip install bpy' -ForegroundColor Red; exit 1 }
    & python $script @a
    if ($LASTEXITCODE -ne 0) { exit 1 }
}
