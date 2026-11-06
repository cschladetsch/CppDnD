# Check that this machine and checkout are ready to build and run. Changes nothing unless asked.
#   .\check.ps1                        report only
#   .\check.ps1 -Model llama3.1        also check Ollama and that model
#   .\check.ps1 -Port 9000             check a different server port
#   .\check.ps1 -Build                 also build dnd_sim and dnd_server
#   .\check.ps1 -Test                  also build and run every test
# Exit code is 0 when nothing failed (warnings are fine), 1 otherwise.
param(
    [int]$Port = 8088,
    [string]$Model = '',
    [switch]$Build,
    [switch]$Test
)
$ErrorActionPreference = 'Continue'
$src   = Join-Path $PSScriptRoot 'sim'
$buildDir = Join-Path $src 'build'
$script:failed = 0
$script:warned = 0

function Ok($msg)   { Write-Host "  [ ok ] $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "  [warn] $msg" -ForegroundColor Yellow; $script:warned++ }
function Bad($msg)  { Write-Host "  [FAIL] $msg" -ForegroundColor Red;    $script:failed++ }
function Head($msg) { Write-Host "`n$msg" -ForegroundColor Cyan }

# ---- toolchain
Head 'Toolchain'
foreach ($tool in 'cmake', 'ninja', 'clang++', 'git') {
    $c = Get-Command $tool -ErrorAction SilentlyContinue
    if ($c) { Ok "$tool  $($c.Source)" } else { Bad "$tool is not on PATH" }
}
if (Get-Command clang++ -ErrorAction SilentlyContinue) {
    $v = (& clang++ --version 2>&1 | Select-Object -First 1)
    if ($v) { Ok "$v" }
}

# ---- checkout
Head 'Checkout'
$required = @(
    'sim\CMakeLists.txt', 'sim\web\index.html', 'sim\data\classes.json', 'sim\data\weapons.json', 'sim\data\spells.json',
    'sim\data\monsters.json', 'sim\data\characters.json', 'sim\data\modules\ravenmoor.json',
    'sim\third_party', 'run.ps1', 'web.ps1', 'test.ps1'
)
foreach ($f in $required) {
    if (Test-Path (Join-Path $PSScriptRoot $f)) { Ok $f } else { Bad "missing $f" }
}
$mods = @(Get-ChildItem (Join-Path $src 'data\modules') -Filter *.json -ErrorAction SilentlyContinue)
if ($mods.Count) { Ok "$($mods.Count) module file(s): $(($mods | ForEach-Object { $_.BaseName }) -join ', ')" } else { Bad 'no module files in sim\data\modules' }
if ($env:DND_DATA) {
    if (Test-Path $env:DND_DATA) { Ok "DND_DATA = $env:DND_DATA" } else { Bad "DND_DATA points to a missing folder: $env:DND_DATA" }
}

# ---- build
Head 'Build'
if ($Build -or $Test) {
    if (-not (Test-Path (Join-Path $buildDir 'build.ninja'))) {
        cmake -S $src -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++ | Out-Null
        if ($LASTEXITCODE) { Bad 'cmake configure failed' }
    }
    cmake --build $buildDir
    if ($LASTEXITCODE) { Bad 'build failed' } else { Ok 'build succeeded' }
}
foreach ($exe in 'dnd_sim', 'dnd_server', 'dnd_tests') {
    $p = Join-Path $buildDir "$exe.exe"
    if (Test-Path $p) { Ok "$exe.exe  ($((Get-Item $p).LastWriteTime.ToString('yyyy-MM-dd HH:mm')))" }
    else { Warn "$exe.exe not built yet (run .\run.ps1, .\web.ps1 or .\check.ps1 -Build)" }
}

# ---- content loads (the program validates every file at startup)
$sim = Join-Path $buildDir 'dnd_sim.exe'
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
try {
    $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
    $probe.Start(); $probe.Stop()
    Ok "port $Port is free"
} catch {
    $running = $null
    try { $running = Invoke-RestMethod -Uri "http://127.0.0.1:$Port/api/llm" -TimeoutSec 2 } catch {}
    if ($running) { Warn "a dnd_server is already running on port $Port (model enabled: $($running.enabled))" }
    else { Bad "port $Port is in use by another program" }
}

# ---- Ollama
Head 'Ollama'
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
