#!/usr/bin/env pwsh
# Check every dependency, set up what is missing, start what is not running, build, serve, open the browser.
# PowerShell 7 on Windows, Linux or macOS (or Windows PowerShell 5.1). Installs use winget, apt/dnf/pacman/zypper or Homebrew.
#   ./web.ps1                          everything: http://127.0.0.1:8088/
#   ./web.ps1 -Yes                     do not ask before installing or downloading anything
#   ./web.ps1 -Port 9000
#   ./web.ps1 -Model llama3.1          use this Ollama model for the plain-English prompt (default: the model in data/agents.json)
#   ./web.ps1 -NoLlm                   no Ollama at all (no plain-English prompt, no AI table)
#                                      (with DND_LLM_URL set and no -Model, the server there is used and Ollama is left alone)
#   ./web.ps1 -Mature                  adult content: no refusals, all characters adults; uses content.model from agents.json
#                                      (an uncensored model such as dolphin-llama3:8b) unless -Model is given
#                                      pictures too: the "mature" block of sim/data/images.json (its checkpoint, words, size)
#                                      is used, and Stable Diffusion is started with that checkpoint (sd.ps1 -Mature); the shipped
#                                      one (Realistic Vision 5.1, 2.1 GB) is offered for download the first time. Pictures follow
#                                      the story: a camera shot draws the latest narration, and every person drawn is an adult
#                                      with "llama" in the content block of agents.json, a llama.cpp server (llama-server) is started on that
#                                      GGUF model instead, kept in the model store (-NoLlama skips this and uses Ollama)
#   ./web.ps1 -NoArt                   no Stable Diffusion (no generated pictures)
#   ./web.ps1 -Rebuild -NoBrowser
#   ./web.ps1 --data ./my-content      unrecognised options go to dnd_server
# What it checks, in order, and what it does about each:
#   cmake, ninja, C++ compiler missing: offers to install with the package manager (clang++ on Windows)
#   llama.cpp                  mature mode only: offers to install it, downloads the GGUF into the model store, starts llama-server
#   Ollama                     missing: offers to install; installed but not running: starts it
#   Ollama models              the prompt model and the models in agents.json; missing: offers to pull them
#   Stable Diffusion           not set up: offers to run sd.ps1 -Install; set up: starts it in the background
#   dnd_server                 builds it if needed, starts it, waits until it answers, opens the browser
# Anything this script started (llama-server, Ollama, Stable Diffusion, the server) is stopped again on Ctrl-C.
# -Art is accepted and does nothing (Stable Diffusion is on by default).
param(
    [int]$Port = 8088,
    [string]$Model = '',
    [switch]$NoLlm,
    [switch]$NoLlama,
    [switch]$Mature,
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
function Fail($msg) {
    # a failure after a model server was started must not leave it running (it holds gigabytes)
    if ($llamaProc -and -not $llamaProc.HasExited) { Stop-Tree $llamaProc.Id }
    if ($ollamaProc -and -not $ollamaProc.HasExited) { Stop-Tree $ollamaProc.Id }
    Restore-Cnf; Write-Host "  [FAIL] $msg" -ForegroundColor Red; exit 1 }
function Ask($question) {
    if ($Yes) { return $true }
    $answer = ''
    try { $answer = Read-Host "$question [y/N]" } catch {}
    return ($answer -match '^(y|yes)$')
}
. (Join-Path (Join-Path $PSScriptRoot 'scripts') 'common.ps1')   # Have, PathOf, Exe, Install-Package, Stop-Tree, ...
function Offer-Install($what, $pkg) {
    $n = Get-PackageName $pkg
    if (-not $n) { return }
    if (Ask "$what is not installed. Install it ($(Get-InstallHint $pkg))?") { Install-Package $pkg | Out-Null }
}

$src   = PathOf $PSScriptRoot 'sim'
$build = PathOf $src 'build'
$url   = "http://127.0.0.1:$Port/"

if (-not (Test-Path (PathOf $src 'web' 'index.html'))) { Fail 'sim/web/index.html is missing' }
if ($Port -lt 1 -or $Port -gt 65535) { Fail "invalid port $Port" }

# ---- 1. build tools
Head 'Build tools'
$pf = if ($OnWindows) { $env:ProgramFiles } else { '' }
$cxx = if ($OnWindows) { 'clang++' } elseif ($env:CXX) { $env:CXX } else { 'c++' }
$tools = @(
    @{ Name = 'cmake'; Dir = $(if ($pf) { PathOf $pf 'CMake' 'bin' });
       Pkg = @{ winget = 'Kitware.CMake'; apt = 'cmake'; dnf = 'cmake'; pacman = 'cmake'; zypper = 'cmake'; brew = 'cmake' } },
    @{ Name = 'ninja'; Dir = '';
       Pkg = @{ winget = 'Ninja-build.Ninja'; apt = 'ninja-build'; dnf = 'ninja-build'; pacman = 'ninja'; zypper = 'ninja'; brew = 'ninja' } },
    @{ Name = $cxx; Dir = $(if ($pf) { PathOf $pf 'LLVM' 'bin' });   # macOS: the Xcode command line tools (xcode-select --install)
       Pkg = @{ winget = 'LLVM.LLVM'; apt = 'g++'; dnf = 'gcc-c++'; pacman = 'gcc'; zypper = 'gcc-c++' } }
)
foreach ($t in $tools) {
    if (-not (Have $t.Name)) { Add-PathIfThere $t.Dir }
    if (-not (Have $t.Name)) { Offer-Install $t.Name $t.Pkg; Add-PathIfThere $t.Dir }
    if (Have $t.Name) { Ok "$($t.Name)  $((Get-Command $t.Name).Source)" }
    elseif ($OnMac -and $t.Name -eq $cxx) { Fail "'$cxx' is not available. Run: xcode-select --install, then run this again." }
    else { Fail "'$($t.Name)' is not available. Install it ($(Get-InstallHint $t.Pkg)), open a new terminal and run this again." }
}

# ---- 2. the port
if (Test-PortFree $Port) { Ok "port $Port is free" }
else {
    $running = $null
    try { $running = Invoke-RestMethod -Uri "${url}api/llm" -TimeoutSec 2 } catch {}
    if ($running) { Fail "port $Port already has a dnd_server running; open $url or pick another -Port" }
    Fail "port $Port is in use by another program; pick another -Port"
}

# ---- 3. build, before llama-server and Stable Diffusion start: together they hold most of the RAM, and the compiler needs some
Head 'Build'
if ($Rebuild -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
if (Invoke-Configure $src) { Fail 'cmake configure failed' }
cmake --build $build --target dnd_server
if ($LASTEXITCODE) { Fail "build failed" }
$exe = PathOf $build (Exe 'dnd_server')
if (-not (Test-Path $exe)) { Fail "$exe was not produced" }

# Everything from here on may start something (llama-server, Ollama, Stable Diffusion, the server); the finally at the end
# stops what this script started, also when it fails or is interrupted, and puts DND_LLM_URL back as it was.
$llmUrlSaved = $env:DND_LLM_URL
$llamaProc = $null; $ollamaProc = $null; $proc = $null; $sdWeStarted = $false; $sdScript = PathOf $PSScriptRoot 'sd.ps1'
try {

# ---- 4. llama.cpp: a GGUF model from the model store (content.llama in agents.json, mature mode)
if (-not $NoLlm -and -not $NoLlama -and -not $env:DND_LLM_URL) {
    $agentsFile0 = if ($env:DND_AGENTS) { $env:DND_AGENTS } else { PathOf $src 'data' 'agents.json' }
    $llamaSpec = ''; $llamaLayers = 30; $llamaPort = 8081; $llamaCtx = 8192
    if (Test-Path $agentsFile0) {
        try {
            $ag0 = Read-JsonFile $agentsFile0
            if ($ag0.content -and $ag0.content.mature) { $Mature = $true }
            if ($Mature -and $ag0.content -and $ag0.content.llama) {
                $llamaSpec = "$($ag0.content.llama)"
                if ($ag0.content.llamaLayers -ne $null) { $llamaLayers = [int]$ag0.content.llamaLayers }
                if ($ag0.content.llamaPort) { $llamaPort = [int]$ag0.content.llamaPort }
                if ($ag0.content.llamaContext) { $llamaCtx = [int]$ag0.content.llamaContext }
            }
        } catch { Warn "could not read $agentsFile0 ($($_.Exception.Message))" }
    }
    if ($llamaSpec) {
        Head 'llama.cpp'
        $llamaPkg = @{ winget = 'ggml.llamacpp'; brew = 'llama.cpp' }   # elsewhere: a release build from github.com/ggml-org/llama.cpp on PATH
        if (-not (Have 'llama-server')) { Offer-Install 'llama-server (llama.cpp)' $llamaPkg }
        if (-not (Have 'llama-server')) {
            $how = if (Get-PackageName $llamaPkg) { Get-InstallHint $llamaPkg } else { 'put llama-server from a github.com/ggml-org/llama.cpp release on PATH' }
            Warn "llama-server is not available ($how, then open a new terminal); using Ollama instead"
        }
        else {
            $llamaUrl = "http://127.0.0.1:$llamaPort"
            $llamaDir = PathOf (Get-ModelHome) (($llamaSpec -split '[/:]')[1])
            function Test-Llama { try { Invoke-RestMethod -Uri "$llamaUrl/health" -TimeoutSec 2 | Out-Null; return $true } catch { return $false } }
            if (Test-Llama) { Ok "llama-server already answering at $llamaUrl"; $env:DND_LLM_URL = $llamaUrl }
            else {
                New-Item -ItemType Directory -Force $llamaDir | Out-Null
                $cached = @(Get-ChildItem $llamaDir -Filter *.gguf -ErrorAction SilentlyContinue).Count -gt 0
                if ($cached -or (Ask "Download $llamaSpec into $llamaDir (about 7 GB for a 12B model)?")) {
                    $env:LLAMA_CACHE = $llamaDir
                    $llamaLog = PathOf $llamaDir 'llama-server.log'
                    Info "starting llama-server on $llamaSpec (log: $llamaLog)"
                    $llamaProc = Start-Background -FilePath (Get-Command 'llama-server').Source -ArgumentList @('-hf', $llamaSpec, '--port', "$llamaPort", '-ngl', "$llamaLayers", '-c', "$llamaCtx") `
                        -Log (PathOf $llamaDir 'llama-server.out.log') -ErrLog $llamaLog
                    $up = $false
                    for ($i = 0; $i -lt 1800 -and -not $up -and -not $llamaProc.HasExited; $i++) {
                        Start-Sleep -Seconds 2
                        $up = Test-Llama
                        if ($i % 15 -eq 14 -and -not $up) { $tail = (Get-Content $llamaLog -Tail 1 -ErrorAction SilentlyContinue); Info "still starting (a first run downloads the model): $tail" }
                    }
                    if ($up) { Ok "llama-server ready at $llamaUrl"; $env:DND_LLM_URL = $llamaUrl }
                    else {
                        Warn "llama-server did not come up (see $llamaLog); using Ollama instead"
                        if (-not $llamaProc.HasExited) { Stop-Tree $llamaProc.Id }
                        $llamaProc = $null
                    }
                } else { Warn 'not downloading; using Ollama instead' }
            }
        }
    }
}

# ---- 5. Ollama and its models
if ($NoLlm) { $Model = '' }
elseif ($env:DND_LLM_URL -and -not $Model) {
    Head 'Language model'
    Ok "using the server in DND_LLM_URL ($env:DND_LLM_URL); Ollama is not touched"
    # the agents' and -Mature's Ollama model names do not apply to it: it answers with whatever it has loaded
    $served = ''
    try { $served = "$((Invoke-RestMethod -Uri "$($env:DND_LLM_URL.TrimEnd('/'))/v1/models" -TimeoutSec 3).data[0].id)" } catch {}
    if ($served) { Ok "it serves '$served'" } else { Warn "could not ask $env:DND_LLM_URL which model it serves (/v1/models)" }
    if ($Mature -and $llamaSpec -and $served -and ($served -notmatch [regex]::Escape((($llamaSpec -split '[/:]')[1])))) {
        Warn "-Mature wants '$llamaSpec' (content.llama in agents.json), but that server serves '$served'; stop it, or unset DND_LLM_URL, and run this again"
    }
}
else {
    Head 'Ollama'
    $ollamaUrl = 'http://127.0.0.1:11434'
    $agentsFile = if ($env:DND_AGENTS) { $env:DND_AGENTS } else { PathOf $src 'data' 'agents.json' }
    $wanted = @()
    $defaultModel = 'qwen2.5:7b'
    if (Test-Path $agentsFile) {
        try {
            $ag = Read-JsonFile $agentsFile
            if ($ag.defaults -and $ag.defaults.model) { $defaultModel = $ag.defaults.model }
            if ($ag.content -and $ag.content.mature) { $Mature = $true }
            if ($Mature -and $ag.content -and $ag.content.model) { $defaultModel = $ag.content.model }
            $nodes = @($ag.defaults, $ag.dm)
            foreach ($section in 'monsterTypes', 'monsters', 'players', 'npcTypes', 'npcs') {
                if ($ag.$section) { foreach ($prop in $ag.$section.PSObject.Properties) { $nodes += $prop.Value } }
            }
            foreach ($node in $nodes) {
                if ($node -and $node.model) { $wanted += "$($node.model)" }
                if ($Mature -and $node -and $node.matureModel) { $wanted += "$($node.matureModel)" }
            }
        } catch { Warn "could not read $agentsFile ($($_.Exception.Message)); its models are not checked" }
    }
    if (-not $Model) { $Model = $defaultModel }
    $wanted = @(@($Model) + $wanted | Where-Object { $_ } | Select-Object -Unique)

    function Get-OllamaTags { try { return (Invoke-RestMethod -Uri "$ollamaUrl/api/tags" -TimeoutSec 3) } catch { return $null } }
    $tags = Get-OllamaTags
    if (-not $tags) {
        $ollamaDir = if ($OnWindows) { PathOf $env:LOCALAPPDATA 'Programs' 'Ollama' } elseif ($OnMac) { '/Applications/Ollama.app/Contents/Resources' } else { '' }
        Add-PathIfThere $ollamaDir
        if (-not (Have 'ollama')) {
            # Linux has no distribution package; ollama.com's install script is the supported way (it also sets up a service)
            $ollamaPkg = @{ winget = 'Ollama.Ollama'; brew = 'ollama' }
            if (Get-PackageName $ollamaPkg) { Offer-Install 'Ollama' $ollamaPkg }
            elseif ($OnLinux -and (Have 'curl') -and (Ask 'Ollama is not installed. Install it with the script from ollama.com (curl -fsSL https://ollama.com/install.sh | sh, uses sudo)?')) {
                & sh -c 'curl -fsSL https://ollama.com/install.sh | sh'
            }
            Refresh-Path
            Add-PathIfThere $ollamaDir
        }
        if (Have 'ollama') {
            Info 'Ollama is not running; starting "ollama serve"'
            $ollamaProc = Start-Background -FilePath (Get-Command 'ollama').Source -ArgumentList @('serve')
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

# ---- 6. Stable Diffusion
$imgUrl = 'http://127.0.0.1:7860'
$matureCkpt = ''
$sdMature = [bool]$Mature
$imgFile = PathOf $src 'data' 'images.json'
if (Test-Path $imgFile) {
    try { $ij = Read-JsonFile $imgFile; if ($ij.url) { $imgUrl = $ij.url }; if ($ij.mature -and $ij.mature.checkpoint) { $matureCkpt = "$($ij.mature.checkpoint)" }; if ($ij.mature -and $ij.mature.enabled) { $sdMature = $true } }
    catch { Warn "could not read sim/data/images.json: $($_.Exception.Message)" }
}
$sdFailed = $false
if (-not $NoArt) {
    Head 'Stable Diffusion'
    if ($sdMature -and $matureCkpt) { Info "mature pictures: checkpoint '$matureCkpt' (sim/data/images.json)" }
    elseif ($sdMature) { Warn 'mature pictures: sim/data/images.json names no "mature" checkpoint, so the base model draws' }
    $sdUp = $false
    try { Invoke-RestMethod -Uri "$imgUrl/sdapi/v1/options" -TimeoutSec 2 | Out-Null; $sdUp = $true } catch {}
    if ($sdUp) {
        Ok "already running at $imgUrl"
        if ($sdMature -and $matureCkpt) {
            $loaded = ''
            try { $loaded = "$((Invoke-RestMethod -Uri "$imgUrl/sdapi/v1/options" -TimeoutSec 3).sd_model_checkpoint)" } catch {}
            $wf = Get-ChildItem (PathOf (Get-ModelHome) $matureCkpt) -Recurse -File -Include '*.safetensors', '*.ckpt' -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($loaded -and $wf -and ($loaded -notlike "*$($wf.BaseName)*")) {
                Warn "it has '$loaded' loaded, not the mature checkpoint '$matureCkpt': run ./sd.ps1 -Stop (or close the server you started), then this again"
            } elseif (-not $wf) {
                Warn "the mature checkpoint '$matureCkpt' is not downloaded, so '$loaded' draws; run ./sd.ps1 -Stop, then this again to be offered the download"
            }
        }
    }
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
            } else { Info 'skipped. ./sd.ps1 -Install sets it up later; -NoArt stops this question' }
        }
        if ($sdReady) {
            # -Mature: sd.ps1 loads the mature checkpoint, offering to download the shipped default (sim/data/images.json) when it is
            # missing, and falls back to the base model if you say no; the game still draws adults only either way
            $sdArgs = @{ Start = $true; NoWait = $true; Port = $sdPort }
            if ($Yes) { $sdArgs.Yes = $true }
            if ($sdMature -and $matureCkpt) { $sdArgs.Mature = $true; $sdArgs.MatureModel = $matureCkpt }
            & $sdScript @sdArgs
            if ($LASTEXITCODE) { Warn 'Stable Diffusion did not start (see above); continuing without pictures'; $sdFailed = $true }
            else { $sdWeStarted = $true }
        }
    }
}

# ---- 7. serve
$argList = @('--port', "$Port")
if ($Model) { $argList += @('--ollama', $Model) }
if ($Mature) { $argList += @('--mature'); if ($Model) { $argList += @('--mature-model', $Model) } }
if ($ServerArgs) { $argList += ($ServerArgs | Where-Object { $_ -ne '--' }) }
$proc = Start-Process -FilePath $exe -ArgumentList @($argList | ForEach-Object { Quote-Arg $_ }) -NoNewWindow -PassThru
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
    elseif ($env:DND_LLM_URL) { Write-Host "  Prompt: plain English through the language model server at $env:DND_LLM_URL" -ForegroundColor Green }
    else { Write-Host '  Prompt: off (no Ollama model)' -ForegroundColor DarkGray }
    try {
        $llm = Invoke-RestMethod -Uri "${url}api/llm" -TimeoutSec 5
        if ($llm.mature) { Write-Host "  Content: mature (adults only), model $($llm.model)" -ForegroundColor Green }
        else { Write-Host '  Content: standard (models may refuse adult content; -Mature turns it on)' -ForegroundColor DarkGray }
    } catch {}
    try {
        $img = Invoke-RestMethod -Uri "${url}api/images" -TimeoutSec 5
        if (-not $img.enabled) { Write-Host "  Pictures: off ($($img.error))" -ForegroundColor DarkGray }
        elseif ($img.problem) { Write-Host "  Pictures: $($img.problem)" -ForegroundColor Yellow }
        elseif ($img.reachable) { Write-Host "  Pictures: image server ready at $($img.url), model '$($img.checkpoint)'$(if ($img.mature) { ', mature (adults only)' })" -ForegroundColor Green }
        elseif ($sdFailed) { Write-Host '  Pictures: Stable Diffusion is not running (the reason is above; ./sd.ps1 -Check lists what is missing)' -ForegroundColor Red }
        elseif ($sdWeStarted) { Write-Host "  Pictures: Stable Diffusion is starting at $($img.url) (the first start takes 10-20 minutes; progress is in sd.log; ./sd.ps1 -Check shows where); the Pictures panel works once it answers" -ForegroundColor Yellow }
        else { Write-Host "  Pictures: no image server at $($img.url) (skipped, or -NoArt)" -ForegroundColor DarkGray }
        if ($img.note) { Write-Host "  Pictures: $($img.note)" -ForegroundColor Yellow }
    } catch {}
    if (-not $NoBrowser) { Open-Url $url }
    $proc.WaitForExit()
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
}

} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    if ($sdWeStarted) { & $sdScript -Stop | Out-Null }
    if ($ollamaProc -and -not $ollamaProc.HasExited) { Stop-Tree $ollamaProc.Id }
    if ($llamaProc -and -not $llamaProc.HasExited) { Stop-Tree $llamaProc.Id }
    $env:DND_LLM_URL = $llmUrlSaved
    Restore-Cnf
}
