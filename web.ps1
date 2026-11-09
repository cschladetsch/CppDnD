# Build (if needed), start the web server, wait until it answers, and open the browser.
#   .\web.ps1                          http://127.0.0.1:8088/
#   .\web.ps1 -Port 9000
#   .\web.ps1 -Model llama3.1          also enable the plain-English prompt through Ollama
#   .\web.ps1 -NoArt                  do not start Stable Diffusion (pictures are on by default; set them up once with .\sd.ps1 -Install)
#   .\web.ps1 -Rebuild -NoBrowser
#   .\web.ps1 --data C:\my\content    unrecognised options go to dnd_server
# Stable Diffusion is started in the background for the Pictures panel unless -NoArt is given;
# if it is not installed the game starts anyway and says so. -Art is accepted and does nothing (it is the default).
# Ctrl-C stops the server (and Stable Diffusion, if this script started it).
param(
    [int]$Port = 8088,
    [string]$Model = '',
    [switch]$Rebuild,
    [switch]$Art,
    [switch]$NoArt,
    [switch]$NoBrowser,
    [Parameter(ValueFromRemainingArguments = $true)] [string[]]$ServerArgs
)
$ErrorActionPreference = 'Stop'
function Fail($msg) { Write-Host "web.ps1: $msg" -ForegroundColor Red; exit 1 }

$src   = Join-Path $PSScriptRoot 'sim'
$build = Join-Path $src 'build'
$url   = "http://127.0.0.1:$Port/"

# ---- tools
foreach ($tool in 'cmake', 'ninja', 'clang++') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Fail "'$tool' is not on PATH" }
}
if (-not (Test-Path (Join-Path $src 'web\index.html'))) { Fail "sim\web\index.html is missing" }
if ($Port -lt 1 -or $Port -gt 65535) { Fail "invalid port $Port" }

# ---- port free?
try {
    $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
    $probe.Start(); $probe.Stop()
} catch {
    $running = $null
    try { $running = Invoke-RestMethod -Uri "${url}api/llm" -TimeoutSec 2 } catch {}
    if ($running) { Fail "port $Port already has a dnd_server running; open $url or pick another -Port" }
    Fail "port $Port is in use by another program; pick another -Port"
}

# ---- Ollama (only when asked for)
if ($Model) {
    try {
        $tags = Invoke-RestMethod -Uri 'http://127.0.0.1:11434/api/tags' -TimeoutSec 3
    } catch {
        Fail "Ollama is not reachable on 127.0.0.1:11434. Start it with 'ollama serve' (or drop -Model to run without the prompt)."
    }
    $have = @($tags.models | ForEach-Object { $_.name })
    if (($have -notcontains $Model) -and ($have -notcontains "${Model}:latest")) {
        $list = if ($have) { $have -join ', ' } else { 'none' }
        Fail "Ollama has no model '$Model' (installed: $list). Run: ollama pull $Model"
    }
    Write-Host "Ollama ok, model $Model"
}

# ---- pictures: where the image server is expected (data\images.json), and start it when asked
$imgUrl = 'http://127.0.0.1:7860'
$imgFile = Join-Path $src 'data\images.json'
if (Test-Path $imgFile) {
    try {
        $ij = (Get-Content $imgFile -Raw) -replace '(?m)^\s*//.*$', '' | ConvertFrom-Json
        if ($ij.url) { $imgUrl = $ij.url }
    } catch { Write-Host "web.ps1: warning: could not read sim\data\images.json: $($_.Exception.Message)" -ForegroundColor Yellow }
}
$sdWeStarted = $false
if (-not $NoArt) {
    $sdScript = Join-Path $PSScriptRoot 'sd.ps1'
    $sdUp = $false
    try { Invoke-RestMethod -Uri "$imgUrl/sdapi/v1/options" -TimeoutSec 2 | Out-Null; $sdUp = $true } catch {}
    if ($sdUp) { Write-Host "Stable Diffusion already running at $imgUrl" }
    elseif (-not (Test-Path $sdScript)) { Write-Host 'web.ps1: warning: sd.ps1 is missing; continuing without pictures' -ForegroundColor Yellow }
    else {
        $sdPort = ([uri]$imgUrl).Port
        & $sdScript -Start -NoWait -Port $sdPort
        if ($LASTEXITCODE) { Write-Host 'web.ps1: warning: Stable Diffusion did not start (run .\sd.ps1 -Install the first time, or .\sd.ps1 -Check to see what is missing; -NoArt skips this); continuing without pictures' -ForegroundColor Yellow }
        else { $sdWeStarted = $true }
    }
}

# ---- build
if ($Rebuild -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    cmake -S $src -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
    if ($LASTEXITCODE) { Fail "cmake configure failed" }
}
cmake --build $build --target dnd_server
if ($LASTEXITCODE) { Fail "build failed" }

$exe = Join-Path $build 'dnd_server.exe'
if (-not (Test-Path $exe)) { Fail "$exe was not produced" }

# ---- start
$argList = @('--port', "$Port")
if ($Model) { $argList += @('--ollama', $Model) }
if ($ServerArgs) { $argList += ($ServerArgs | Where-Object { $_ -ne '--' }) }
$proc = Start-Process -FilePath $exe -ArgumentList $argList -NoNewWindow -PassThru
try {
    $ready = $false
    for ($i = 0; $i -lt 60 -and -not $proc.HasExited; $i++) {
        try { Invoke-RestMethod -Uri "${url}api/llm" -TimeoutSec 1 | Out-Null; $ready = $true; break }
        catch { Start-Sleep -Milliseconds 250 }
    }
    if ($proc.HasExited) { Fail "dnd_server exited with code $($proc.ExitCode)" }
    if (-not $ready)     { Fail "dnd_server did not answer on $url within 15 seconds" }

    Write-Host "Ready: $url   (Ctrl-C to stop)" -ForegroundColor Green
    try {
        $img = Invoke-RestMethod -Uri "${url}api/images" -TimeoutSec 5
        if (-not $img.enabled) { Write-Host "Pictures: off ($($img.error))" -ForegroundColor DarkGray }
        elseif ($img.problem) { Write-Host "Pictures: $($img.problem)" -ForegroundColor Yellow }
        elseif ($img.reachable) { Write-Host "Pictures: image server ready at $($img.url), model '$($img.checkpoint)'" -ForegroundColor Green }
        elseif ($sdWeStarted) { Write-Host "Pictures: Stable Diffusion is starting at $($img.url) (the first start takes 10-20 minutes); the Pictures panel works once it answers" -ForegroundColor Yellow }
        else { Write-Host "Pictures: no image server at $($img.url). .\sd.ps1 -Install sets one up (it was skipped or failed to start, or you used -NoArt)." -ForegroundColor DarkGray }
    } catch {}
    if (-not $NoBrowser) { Start-Process $url }
    $proc.WaitForExit()
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    if ($sdWeStarted) { & (Join-Path $PSScriptRoot 'sd.ps1') -Stop | Out-Null }
}
