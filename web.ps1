# Check every dependency, set up what is missing, start what is not running, build, serve, open the browser.
#   .\web.ps1                          everything: http://127.0.0.1:8088/
#   .\web.ps1 -Yes                     do not ask before installing or downloading anything
#   .\web.ps1 -Port 9000
#   .\web.ps1 -Model llama3.1          use this Ollama model for the plain-English prompt (default: the model in data\agents.json)
#   .\web.ps1 -NoLlm                   no Ollama at all (no plain-English prompt, no AI table)
#                                      (with DND_LLM_URL set and no -Model, the server there is used and Ollama is left alone)
#   .\web.ps1 -NoArt                   no Stable Diffusion (no generated pictures)
#   .\web.ps1 -Rebuild -NoBrowser
#   .\web.ps1 --data C:\my\content     unrecognised options go to dnd_server
# What it checks, in order, and what it does about each:
#   cmake, ninja, clang++      missing: offers to install with winget
#   Ollama                     missing: offers to install; installed but not running: starts it
#   Ollama models              the prompt model and the models in agents.json; missing: offers to pull them
#   Stable Diffusion           not set up: offers to run sd.ps1 -Install; set up: starts it in the background
#   dnd_server                 builds it if needed, starts it, waits until it answers, opens the browser
# Anything this script started (Ollama, Stable Diffusion, the server) is stopped again on Ctrl-C.
# -Art is accepted and does nothing (Stable Diffusion is on by default).
param(
    [int]$Port = 8088,
    [string]$Model = '',
    [switch]$NoLlm,
    [switch]$NoArt,
    [switch]$Art,
    [switch]$Yes,
    [switch]$Rebuild,
    [switch]$NoBrowser,
    [Parameter(ValueFromRemainingArguments = $true)] [string[]]$ServerArgs
)
$ErrorActionPreference = 'Stop'

# A profile may install a command-not-found handler that prints a line for every probe below; mute it while this script runs.
$cnfSaved = $ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction
$ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction = $null
function Restore-Cnf { $ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction = $cnfSaved }

function Head($msg) { Write-Host "`n$msg" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "  [ ok ] $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "  [warn] $msg" -ForegroundColor Yellow }
function Info($msg) { Write-Host "         $msg" -ForegroundColor DarkGray }
function Fail($msg) { Restore-Cnf; Write-Host "  [FAIL] $msg" -ForegroundColor Red; exit 1 }
function Ask($question) {
    if ($Yes) { return $true }
    $answer = ''
    try { $answer = Read-Host "$question [y/N]" } catch {}
    return ($answer -match '^(y|yes)$')
}
function Have($name) { return [bool](Get-Command $name -ErrorAction SilentlyContinue) }
function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
function Add-PathIfThere($dir) {
    if ($dir -and (Test-Path $dir) -and (($env:Path -split ';') -notcontains $dir)) { $env:Path = "$env:Path;$dir" }
}
function Read-JsonFile($file) {
    return ((Get-Content $file -Raw) -replace '(?s)/\*.*?\*/', '' -replace '(?m)^\s*//.*$', '') | ConvertFrom-Json
}
function Stop-Tree($id) { & taskkill.exe /PID $id /T /F 2>&1 | Out-Null }

$src   = Join-Path $PSScriptRoot 'sim'
$build = Join-Path $src 'build'
$url   = "http://127.0.0.1:$Port/"

if (-not (Test-Path (Join-Path $src 'web\index.html'))) { Fail "sim\web\index.html is missing" }
if ($Port -lt 1 -or $Port -gt 65535) { Fail "invalid port $Port" }

# ---- 1. build tools
Head 'Build tools'
$tools = @(
    @{ Name = 'cmake';   Id = 'Kitware.CMake';     Dir = 'C:\Program Files\CMake\bin' },
    @{ Name = 'ninja';   Id = 'Ninja-build.Ninja'; Dir = '' },
    @{ Name = 'clang++'; Id = 'LLVM.LLVM';         Dir = 'C:\Program Files\LLVM\bin' }
)
foreach ($t in $tools) {
    if (-not (Have $t.Name)) { Add-PathIfThere $t.Dir }
    if (-not (Have $t.Name) -and (Have 'winget')) {
        if (Ask "$($t.Name) is not installed. Install it with winget ($($t.Id))?") {
            winget install -e --id $t.Id --silent --accept-package-agreements --accept-source-agreements
            Refresh-Path
            Add-PathIfThere $t.Dir
        }
    }
    if (Have $t.Name) { Ok "$($t.Name)  $((Get-Command $t.Name).Source)" }
    else { Fail "'$($t.Name)' is not available. Install it (winget install $($t.Id)), open a new terminal and run this again." }
}

# ---- 2. the port
try {
    $probe = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
    $probe.Start(); $probe.Stop()
    Ok "port $Port is free"
} catch {
    $running = $null
    try { $running = Invoke-RestMethod -Uri "${url}api/llm" -TimeoutSec 2 } catch {}
    if ($running) { Fail "port $Port already has a dnd_server running; open $url or pick another -Port" }
    Fail "port $Port is in use by another program; pick another -Port"
}

# ---- 3. Ollama and its models
$ollamaProc = $null
if ($NoLlm) { $Model = '' }
elseif ($env:DND_LLM_URL -and -not $Model) {
    Head 'Language model'
    Ok "using the server in DND_LLM_URL ($env:DND_LLM_URL); Ollama is not touched"
}
else {
    Head 'Ollama'
    $ollamaUrl = 'http://127.0.0.1:11434'
    $agentsFile = if ($env:DND_AGENTS) { $env:DND_AGENTS } else { Join-Path $src 'data\agents.json' }
    $wanted = @()
    $defaultModel = 'qwen2.5:7b'
    if (Test-Path $agentsFile) {
        try {
            $ag = Read-JsonFile $agentsFile
            if ($ag.defaults -and $ag.defaults.model) { $defaultModel = $ag.defaults.model }
            $nodes = @($ag.defaults, $ag.dm)
            foreach ($section in 'monsterTypes', 'monsters', 'players', 'npcTypes', 'npcs') {
                if ($ag.$section) { foreach ($prop in $ag.$section.PSObject.Properties) { $nodes += $prop.Value } }
            }
            foreach ($node in $nodes) { if ($node -and $node.model) { $wanted += "$($node.model)" } }
        } catch { Warn "could not read $agentsFile ($($_.Exception.Message)); its models are not checked" }
    }
    if (-not $Model) { $Model = $defaultModel }
    $wanted = @(@($Model) + $wanted | Where-Object { $_ } | Select-Object -Unique)

    function Get-OllamaTags { try { return (Invoke-RestMethod -Uri "$ollamaUrl/api/tags" -TimeoutSec 3) } catch { return $null } }
    $tags = Get-OllamaTags
    if (-not $tags) {
        Add-PathIfThere (Join-Path $env:LOCALAPPDATA 'Programs\Ollama')
        if (-not (Have 'ollama') -and (Have 'winget')) {
            if (Ask 'Ollama is not installed. Install it with winget (Ollama.Ollama)?') {
                winget install -e --id Ollama.Ollama --silent --accept-package-agreements --accept-source-agreements
                Refresh-Path
                Add-PathIfThere (Join-Path $env:LOCALAPPDATA 'Programs\Ollama')
            }
        }
        if (Have 'ollama') {
            Info 'Ollama is not running; starting "ollama serve"'
            $ollamaProc = Start-Process -FilePath (Get-Command 'ollama').Source -ArgumentList 'serve' -WindowStyle Hidden -PassThru
            for ($i = 0; $i -lt 60 -and -not $tags; $i++) {
                Start-Sleep -Milliseconds 500
                $tags = Get-OllamaTags
            }
        }
    }
    if (-not $tags) {
        Warn 'Ollama is not available, so the plain-English prompt and the AI table are off (-NoLlm hides this)'
        if ($ollamaProc -and -not $ollamaProc.HasExited) { Stop-Tree $ollamaProc.Id }
        $ollamaProc = $null
        $Model = ''
    } else {
        Ok "Ollama answering at $ollamaUrl"
        $have = @($tags.models | ForEach-Object { $_.name })
        $missing = @($wanted | Where-Object { ($have -notcontains $_) -and ($have -notcontains "${_}:latest") })
        if ($missing.Count -gt 0) {
            if (Ask "Ollama is missing $($missing.Count) model(s): $($missing -join ', '). Pull them now (several GB)?") {
                foreach ($m in $missing) {
                    Write-Host "  pulling $m ..."
                    ollama pull $m
                    if ($LASTEXITCODE) { Warn "could not pull $m" }
                }
                $tags = Get-OllamaTags
                $have = @($tags.models | ForEach-Object { $_.name })
                $missing = @($wanted | Where-Object { ($have -notcontains $_) -and ($have -notcontains "${_}:latest") })
            }
        }
        foreach ($m in $wanted) {
            if ($missing -contains $m) { Warn "model '$m' is not installed (ollama pull $m)" } else { Ok "model '$m'" }
        }
        if ($missing -contains $Model) {
            Warn "the prompt model '$Model' is not installed, so the plain-English prompt is off"
            $Model = ''
        }
    }
}

# ---- 4. Stable Diffusion
$imgUrl = 'http://127.0.0.1:7860'
$imgFile = Join-Path $src 'data\images.json'
if (Test-Path $imgFile) {
    try { $ij = Read-JsonFile $imgFile; if ($ij.url) { $imgUrl = $ij.url } }
    catch { Warn "could not read sim\data\images.json: $($_.Exception.Message)" }
}
$sdWeStarted = $false
$sdFailed = $false
$sdScript = Join-Path $PSScriptRoot 'sd.ps1'
if (-not $NoArt) {
    Head 'Stable Diffusion'
    $sdUp = $false
    try { Invoke-RestMethod -Uri "$imgUrl/sdapi/v1/options" -TimeoutSec 2 | Out-Null; $sdUp = $true } catch {}
    if ($sdUp) { Ok "already running at $imgUrl" }
    elseif (-not (Test-Path $sdScript)) { Warn 'sd.ps1 is missing; continuing without pictures'; $sdFailed = $true }
    else {
        $sdPort = ([uri]$imgUrl).Port
        $sdReady = $true
        & $sdScript -Installed
        if ($LASTEXITCODE) {
            $sdReady = $false
            Warn 'Stable Diffusion is not set up yet'
            if (Ask 'Set it up now (git, Python 3.10, AUTOMATIC1111 and the 4.3 GB model, about 10 GB in all; a long download)?') {
                & $sdScript -Install -Yes -Port $sdPort
                if ($LASTEXITCODE) { Warn 'the Stable Diffusion install did not finish (see above); continuing without pictures'; $sdFailed = $true }
                else { $sdReady = $true }
            } else { Info 'skipped. .\sd.ps1 -Install sets it up later; -NoArt stops this question' }
        }
        if ($sdReady) {
            & $sdScript -Start -NoWait -Port $sdPort
            if ($LASTEXITCODE) { Warn 'Stable Diffusion did not start (see above); continuing without pictures'; $sdFailed = $true }
            else { $sdWeStarted = $true }
        }
    }
}

# ---- 5. build
Head 'Build'
if ($Rebuild -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    cmake -S $src -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
    if ($LASTEXITCODE) { Fail "cmake configure failed" }
}
cmake --build $build --target dnd_server
if ($LASTEXITCODE) { Fail "build failed" }
$exe = Join-Path $build 'dnd_server.exe'
if (-not (Test-Path $exe)) { Fail "$exe was not produced" }

# ---- 6. serve
$argList = @('--port', "$Port")
if ($Model) { $argList += @('--ollama', $Model) }
if ($ServerArgs) { $argList += ($ServerArgs | Where-Object { $_ -ne '--' }) }
$proc = Start-Process -FilePath $exe -ArgumentList $argList -NoNewWindow -PassThru
try {
    $ready = $false
    for ($i = 0; $i -lt 120 -and -not $proc.HasExited; $i++) {
        try { Invoke-RestMethod -Uri "${url}api/llm" -TimeoutSec 1 | Out-Null; $ready = $true; break }
        catch { Start-Sleep -Milliseconds 250 }
    }
    if ($proc.HasExited) { Fail "dnd_server exited with code $($proc.ExitCode)" }
    if (-not $ready)     { Fail "dnd_server did not answer on $url within 30 seconds" }

    Head 'Ready'
    Write-Host "  $url   (Ctrl-C to stop)" -ForegroundColor Green
    if ($Model) { Write-Host "  Prompt: plain English through Ollama, model $Model" -ForegroundColor Green }
    else { Write-Host '  Prompt: off (no Ollama model)' -ForegroundColor DarkGray }
    try {
        $img = Invoke-RestMethod -Uri "${url}api/images" -TimeoutSec 5
        if (-not $img.enabled) { Write-Host "  Pictures: off ($($img.error))" -ForegroundColor DarkGray }
        elseif ($img.problem) { Write-Host "  Pictures: $($img.problem)" -ForegroundColor Yellow }
        elseif ($img.reachable) { Write-Host "  Pictures: image server ready at $($img.url), model '$($img.checkpoint)'" -ForegroundColor Green }
        elseif ($sdFailed) { Write-Host '  Pictures: Stable Diffusion is not running (the reason is above; .\sd.ps1 -Check lists what is missing)' -ForegroundColor Red }
        elseif ($sdWeStarted) { Write-Host "  Pictures: Stable Diffusion is starting at $($img.url) (the first start takes 10-20 minutes; progress is in sd.log under $env:LOCALAPPDATA\CppDnD\sd); the Pictures panel works once it answers" -ForegroundColor Yellow }
        else { Write-Host "  Pictures: no image server at $($img.url) (skipped, or -NoArt)" -ForegroundColor DarkGray }
    } catch {}
    if (-not $NoBrowser) { Start-Process $url }
    $proc.WaitForExit()
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    if ($sdWeStarted) { & $sdScript -Stop | Out-Null }
    if ($ollamaProc -and -not $ollamaProc.HasExited) { Stop-Tree $ollamaProc.Id }
    Restore-Cnf
}
