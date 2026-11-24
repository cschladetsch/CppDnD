#!/usr/bin/env pwsh
# Check that this machine and checkout are ready to build and run. Changes nothing unless asked.
# PowerShell 7 on Windows, Linux or macOS (or Windows PowerShell 5.1).
#   ./check.ps1                        report only
#   ./check.ps1 -Model llama3.1        also check Ollama and that model
#   ./check.ps1 -Port 9000             check a different server port
#   ./check.ps1 -Build                 also build dnd_sim and dnd_server
#   ./check.ps1 -Test                  also build and run every test
# Exit code is 0 when nothing failed (warnings are fine), 1 otherwise.
param(
    [int]$Port = 8088,
    [string]$Model = '',
    [switch]$Build,
    [switch]$Test
)
$ErrorActionPreference = 'Continue'
. (Join-Path (Join-Path $PSScriptRoot 'scripts') 'common.ps1')
$src   = PathOf $PSScriptRoot 'sim'
$buildDir = PathOf $src 'build'
$script:failed = 0
$script:warned = 0

function Ok($msg)   { Write-Host "  [ ok ] $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "  [warn] $msg" -ForegroundColor Yellow; $script:warned++ }
function Bad($msg)  { Write-Host "  [FAIL] $msg" -ForegroundColor Red;    $script:failed++ }
function Head($msg) { Write-Host "`n$msg" -ForegroundColor Cyan }

# ---- toolchain
Head 'Toolchain'
# Windows builds with clang++ (the "clang" preset); elsewhere the default compiler (CXX, else c++) through the "release" preset.
$cxx = if ($OnWindows) { 'clang++' } elseif ($env:CXX) { $env:CXX } else { 'c++' }
foreach ($tool in 'cmake', 'ninja', $cxx, 'git') {
    $c = Get-Command $tool -ErrorAction SilentlyContinue
    if ($c) { Ok "$tool  $($c.Source)" } else { Bad "$tool is not on PATH" }
}
$cv = Get-CMakeVersion
if ($cv -and $cv -lt [version]'3.21') { Bad "cmake $cv is too old; CMakePresets.json needs 3.21 or newer" }
if (Have $cxx) {
    $v = (& $cxx --version 2>&1 | Select-Object -First 1)
    if ($v) { Ok "$v" }
}

# ---- checkout
Head 'Checkout'
$required = @(
    'sim/CMakeLists.txt', 'sim/CMakePresets.json', 'sim/web/index.html', 'sim/data/classes.json', 'sim/data/weapons.json', 'sim/data/spells.json',
    'sim/data/monsters.json', 'sim/data/characters.json', 'sim/data/modules/ravenmoor.json',
    'sim/third_party', 'scripts/common.ps1', 'run.ps1', 'web.ps1', 'test.ps1'
)
foreach ($f in $required) {
    if (Test-Path (Join-Path $PSScriptRoot $f)) { Ok $f } else { Bad "missing $f" }
}
$mods = @(Get-ChildItem (PathOf $src 'data' 'modules') -Filter *.json -ErrorAction SilentlyContinue)
if ($mods.Count) { Ok "$($mods.Count) module file(s): $(($mods | ForEach-Object { $_.BaseName }) -join ', ')" } else { Bad 'no module files in sim/data/modules' }
if ($env:DND_DATA) {
    if (Test-Path $env:DND_DATA) { Ok "DND_DATA = $env:DND_DATA" } else { Bad "DND_DATA points to a missing folder: $env:DND_DATA" }
}

# ---- build
Head 'Build'
if ($Build -or $Test) {
    if (Invoke-Configure $src -Quiet) { Bad "cmake configure failed (cmake --preset $(Get-BuildPreset) in sim shows why)" }
    cmake --build $buildDir
    if ($LASTEXITCODE) { Bad 'build failed' } else { Ok 'build succeeded' }
}
foreach ($exe in 'dnd_sim', 'dnd_server', 'dnd_tests') {
    $p = PathOf $buildDir (Exe $exe)
    if (Test-Path $p) { Ok "$(Exe $exe)  ($((Get-Item $p).LastWriteTime.ToString('yyyy-MM-dd HH:mm')))" }
    else { Warn "$(Exe $exe) not built yet (run ./run.ps1, ./web.ps1 or ./check.ps1 -Build)" }
}

# ---- content loads (the program validates every file at startup)
$sim = PathOf $buildDir (Exe 'dnd_sim')
if (Test-Path $sim) {
    Head 'Content'
    $out = & $sim --list 2>&1
    if ($LASTEXITCODE -eq 0) {
        $m = @($out | Where-Object { $_ -match '^\s+\S+\s+.*\(level' }).Count
        Ok "content loads and validates ($m module(s))"
    } else {
        Bad "content failed to load: $(($out | Select-Object -First 3) -join ' | ')"
    }
}

# ---- port
Head "Port $Port"
if (Test-PortFree $Port) { Ok "port $Port is free" }
else {
    $running = $null
    try { $running = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/llm" -TimeoutSec 2 } catch {}
    if ($running) { Warn "a dnd_server is already running on port $Port (model enabled: $($running.enabled))" }
    else { Bad "port $Port is in use by another program" }
}

# ---- Ollama
Head 'Ollama'
$have = @()
try {
    $tags = Invoke-RestMethod -Uri 'http://127.0.0.1:11434/api/tags' -TimeoutSec 3
    $have = @($tags.models | ForEach-Object { $_.name })
    Ok "reachable on 127.0.0.1:11434, models: $(if ($have) { $have -join ', ' } else { 'none' })"
    if ($Model) {
        if (($have -contains $Model) -or ($have -contains "${Model}:latest")) { Ok "model '$Model' is installed" }
        else { Bad "model '$Model' is not installed (ollama pull $Model)" }
    }
} catch {
    if ($Model) { Bad 'Ollama is not reachable on 127.0.0.1:11434 (ollama serve)' }
    else { Warn 'Ollama is not running; the plain-English prompt will be off (optional)' }
}
if ($env:DND_LLM_URL -or $env:DND_LLM_MODEL) { Ok "DND_LLM_URL='$env:DND_LLM_URL' DND_LLM_MODEL='$env:DND_LLM_MODEL'" }

# ---- agents.json: parses, and the models it names are installed in Ollama
Head 'Agents'
$agentsFile = if ($env:DND_AGENTS) { $env:DND_AGENTS } else { PathOf $src 'data' 'agents.json' }
if (-not (Test-Path $agentsFile)) {
    Warn "no agents file at $agentsFile (the AI table is optional)"
} else {
    try {
        $raw = (Get-Content $agentsFile -Raw) -replace '(?s)/\*.*?\*/', '' -replace '(?m)^\s*//.*$', ''
        $a = $raw | ConvertFrom-Json
        $models = [ordered]@{}
        function AddModel($who, $node) { if ($node -and $node.model) { $models[$who] = $node.model } }
        AddModel 'defaults' $a.defaults
        AddModel 'dm' $a.dm
        foreach ($section in 'monsterTypes', 'monsters', 'players', 'npcTypes', 'npcs') {
            if ($a.$section) { foreach ($p in $a.$section.PSObject.Properties) { AddModel "$section.$($p.Name)" $p.Value } }
        }
        Ok "$agentsFile parses; $($models.Count) model setting(s)"
        if ($have) {
            foreach ($m in ($models.Values | Sort-Object -Unique)) {
                if (($have -contains $m) -or ($have -contains "${m}:latest")) { Ok "model '$m' is installed" }
                else { Warn "model '$m' is named in agents.json but not installed in Ollama (ollama pull $m)" }
            }
        } else {
            Warn 'Ollama models unknown (Ollama not reachable), so agents.json models were not checked'
        }
    } catch {
        Bad "agents.json is not valid: $($_.Exception.Message)"
    }
}

# ---- art: pictures are optional; the client draws its own tokens without them
Head 'Art'
$artDir = PathOf $PSScriptRoot 'sim' 'art'
if (Test-Path $artDir) {
    $counts = foreach ($k in 'characters','monsters','npcs','locations','modules') {
        $n = @(Get-ChildItem (Join-Path $artDir $k) -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.png','.jpg','.jpeg','.webp','.svg' }).Count
        "$k $n"
    }
    Ok ("art folder: " + ($counts -join ', ') + ' (missing pictures fall back to drawn tokens)')
} else { Warn 'sim/art is missing' }
$sd = 'http://127.0.0.1:7860'; $sdModel = ''; $sdBackend = 'a1111'
$imagesFile = PathOf $PSScriptRoot 'sim' 'data' 'images.json'
if (Test-Path $imagesFile) {
    try {
        $ij = (Get-Content $imagesFile -Raw) -replace '(?m)^\s*//.*$', '' | ConvertFrom-Json
        if ($ij.url) { $sd = $ij.url }
        if ($ij.checkpoint) { $sdModel = $ij.checkpoint }
        if ($ij.backend) { $sdBackend = $ij.backend }
        if ($ij.enabled -eq $false) { Warn 'picture generation is turned off in sim/data/images.json' }
    } catch { Bad "sim/data/images.json is not valid: $($_.Exception.Message)" }
}
if ($env:SD_URL) { $sd = $env:SD_URL }
Write-Host '         full Stable Diffusion requirements: ./sd.ps1 -Check   (install: ./sd.ps1 -Install, run: ./web.ps1)' -ForegroundColor DarkGray
try {
    if ($sdBackend -eq 'openai') { Invoke-RestMethod "$sd/v1/models" -TimeoutSec 2 | Out-Null } else { Invoke-RestMethod "$sd/sdapi/v1/options" -TimeoutSec 2 | Out-Null }
    Ok "image server ($sdBackend) answers at $sd (the Pictures panel and ./art.ps1 can draw)"
} catch { Warn "no image server at $sd (optional; start AUTOMATIC1111 or Forge with --api to draw pictures)" }
if ($sdModel) {
    $home_ = Get-ModelHome
    $mdir = Join-Path $home_ $sdModel
    if (Test-Path $mdir) {
        $w = Get-ChildItem $mdir -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Extension -in '.safetensors','.ckpt','.gguf' } | Select-Object -First 1
        if ($w) { Ok "image model '$sdModel' is in the model store ($($w.Name))" } else { Warn "model folder $mdir holds no .safetensors, .ckpt or .gguf" }
    } elseif ($sdModel -eq 'realistic-vision-5.1') { Warn "image model '$sdModel' is not downloaded yet ($mdir); sd15 draws until ./web.ps1 (or ./sd.ps1 -Start) downloads it" }
    else { Warn "image model '$sdModel' is not in the model store ($mdir); the image server must know it by name" }
}

# ---- tests
if ($Test) {
    Head 'Tests'
    ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE) { Bad 'tests failed' } else { Ok 'all tests passed' }
}

Write-Host ''
if ($failed) { Write-Host "$failed check(s) failed, $warned warning(s)." -ForegroundColor Red; exit 1 }
Write-Host "Ready. $warned warning(s)." -ForegroundColor Green
exit 0
