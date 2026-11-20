#!/usr/bin/env pwsh
# Stable Diffusion for CppDnD: check what it needs, install it, start it, stop it.
# PowerShell 7 on Windows, Linux or macOS (or Windows PowerShell 5.1). Windows runs webui.bat, Linux and macOS webui.sh.
# CppDnD draws pictures through an AUTOMATIC1111 server (started with --api). This script sets one up.
#   ./sd.ps1                 same as -Check
#   ./sd.ps1 -Check          report every requirement (changes nothing)
#   ./sd.ps1 -Install        install what is missing: git and Python 3.10 (winget, apt/dnf/pacman/zypper or Homebrew; asks first),
#                            AUTOMATIC1111 (cloned), and the SD 1.5 model (into the CppLmmModelStore)
#   ./sd.ps1 -Start          start the API server in the background and wait until it answers
#                            (the first start installs PyTorch, about 5 GB, and takes 10-20 minutes)
#   ./sd.ps1 -Stop           stop a server this script started
#   -Installed               print nothing; exit 0 if AUTOMATIC1111 and the model are installed, 1 if not (web.ps1 uses this)
#   -NoWait                  with -Start: return as soon as the server is launched (web.ps1 uses this)
#   -Yes                     do not ask before installing a package
#   -Port 7860               API port (must match "url" in sim/data/images.json)
#   -SdHome DIR              where AUTOMATIC1111 lives (default %LOCALAPPDATA%\CppDnD\sd on Windows, ~/.local/share/cppdnd/sd elsewhere, or $env:DND_SD_HOME)
#   -ModelName sd15          the model-store folder that holds the checkpoint (matches "checkpoint" in images.json)
#   -Mature                  with -Start: also load the mature checkpoint (-MatureModel, else "mature"."checkpoint" in images.json)
#   -MatureModel NAME        a model-store folder holding a .safetensors or .ckpt (an SD1.5 or SDXL fine-tune);
#                            AUTOMATIC1111 lists it beside sd15 and starts with it loaded. The default, realistic-vision-5.1, is
#                            offered for download (2.1 GB, plus its 330 MB VAE) when the folder is missing; other folders you fill yourself.
#   -Cpu                     no NVIDIA GPU: run on the CPU (works, but a picture takes minutes). Apple Silicon Macs use the GPU (Metal) without it
#   -Tune auto|none|medvram|alloc   memory tuning for the GPU: auto (default) and medvram use --medvram on cards under 9 GB; none uses no tuning;
#                            alloc sets PYTORCH_CUDA_ALLOC_CONF (garbage_collection_threshold, max_split_size_mb), which crashed the server on one machine
# Environment: DND_SD_HOME, DND_SD_REPO (a fork, e.g. Forge), DND_SD_TAG (default v1.10.1), DND_SD_ARGS (extra launch flags),
#              DEEPSEEK_MODEL_HOME (the model store).
# Exit code 0: fine (or started); 1: a requirement failed.
param(
    [switch]$Check,
    [switch]$Install,
    [switch]$Start,
    [switch]$Stop,
    [switch]$Yes,
    [switch]$Cpu,
    [switch]$NoWait,
    [switch]$Installed,
    [switch]$Mature,
    [string]$MatureModel = '',
    [ValidateSet('auto', 'none', 'medvram', 'alloc')][string]$Tune = 'auto',
    [int]$Port = 7860,
    [string]$SdHome = '',
    [string]$ModelName = 'sd15',
    [int]$TimeoutMin = 40
)
$ErrorActionPreference = 'Continue'
$script:failed = 0
$script:warned = 0
function Ok($msg)   { Write-Host "  [ ok ] $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "  [warn] $msg" -ForegroundColor Yellow; $script:warned++ }
function Bad($msg)  { Write-Host "  [FAIL] $msg" -ForegroundColor Red;    $script:failed++ }
function Head($msg) { Write-Host "`n$msg" -ForegroundColor Cyan }
function Info($msg) { Write-Host "         $msg" -ForegroundColor DarkGray }

. (Join-Path (Join-Path $PSScriptRoot 'scripts') 'common.ps1')   # $OnWindows, PathOf, Install-Package, Stop-Tree, ...
$isWin = $OnWindows
$appleGpu = $OnMac -and ("$(& uname -m 2>$null)".Trim() -eq 'arm64')   # A1111's webui.sh runs on Metal (MPS) there
if (-not ($Check -or $Install -or $Start -or $Stop -or $Installed)) { $Check = $true }

# A profile may install a command-not-found handler that prints a line for every Get-Command probe below; mute it while this script runs.
$cnfSaved = $ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction
$ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction = $null

# ---- where things live
$sdRoot = $SdHome
if (-not $sdRoot) { $sdRoot = $env:DND_SD_HOME }
if (-not $sdRoot) {
    if ($isWin) { $sdRoot = PathOf (Get-DataHome) 'CppDnD' 'sd' } else { $sdRoot = PathOf (Get-DataHome) 'cppdnd' 'sd' }
}
$webui    = PathOf $sdRoot 'stable-diffusion-webui'
$launcher = if ($isWin) { PathOf $webui 'webui.bat' } else { PathOf $webui 'webui.sh' }
$venvPy   = if ($isWin) { PathOf $webui 'venv' 'Scripts' 'python.exe' } else { PathOf $webui 'venv' 'bin' 'python' }
$logFile  = PathOf $sdRoot 'sd.log'
$errFile  = PathOf $sdRoot 'sd.err.log'
$pidFile  = PathOf $sdRoot 'sd.pid'
$repoUrl  = if ($env:DND_SD_REPO) { $env:DND_SD_REPO } else { 'https://github.com/AUTOMATIC1111/stable-diffusion-webui' }
$repoTag  = if ($env:DND_SD_TAG)  { $env:DND_SD_TAG }  else { 'v1.10.1' }
$apiUrl   = "http://127.0.0.1:$Port"

$modelHome  = Get-ModelHome
$modelDir   = PathOf $modelHome $ModelName
$modelFile  = PathOf $modelDir 'v1-5-pruned-emaonly.safetensors'
$modelUrl   = 'https://huggingface.co/stable-diffusion-v1-5/stable-diffusion-v1-5/resolve/main/v1-5-pruned-emaonly.safetensors'
$modelBytes = 4265146304
$minFreeGb  = 15

$gitPkg = @{ winget = 'Git.Git'; apt = 'git'; dnf = 'git'; pacman = 'git'; zypper = 'git'; brew = 'git' }
# Python 3.10 by name: Ubuntu 22.04 and Fedora have it; newer Debian/Ubuntu need the deadsnakes PPA (or pyenv / uv python install 3.10)
$pyPkg  = @{ winget = 'Python.Python.3.10'; apt = @('python3.10', 'python3.10-venv'); dnf = 'python3.10'; brew = 'python@3.10' }

# ---- probes
function Test-Api {
    try { Invoke-RestMethod -Uri "$apiUrl/sdapi/v1/options" -TimeoutSec 3 | Out-Null; return $true } catch { return $false }
}
function Get-Port-Free { return (Test-PortFree $Port) }
function Find-Python310 {
    # AUTOMATIC1111 needs Python 3.10 (3.10.6 recommended); newer Pythons have no matching PyTorch pins.
    $candidates = @()
    if ($isWin) {
        $py = Get-Command py -ErrorAction SilentlyContinue
        if ($py) {
            $p = (& py -3.10 -c "import sys; print(sys.executable)" 2>$null)
            if ($LASTEXITCODE -eq 0 -and $p) { $candidates += "$p".Trim() }
        }
    }
    foreach ($n in 'python3.10', 'python', 'python3') {
        $c = Get-Command $n -ErrorAction SilentlyContinue
        if ($c) { $candidates += $c.Source }
    }
    foreach ($c in $candidates) {
        $v = (& $c --version 2>&1) | Out-String
        if ($v -match 'Python 3\.10\.(\d+)') { return @{ Path = $c; Version = $v.Trim() } }
    }
    return $null
}
function Get-Gpu { return (Get-NvidiaGpu) }
function Get-FreeGb($path) {
    try {
        $full = [System.IO.Path]::GetFullPath($path)
        $d = New-Object System.IO.DriveInfo ([System.IO.Path]::GetPathRoot($full))
        return [math]::Round($d.AvailableFreeSpace / 1GB, 1)
    } catch { return -1 }
}
function Test-Url($u) {
    try { Invoke-WebRequest -Uri $u -Method Head -TimeoutSec 8 -UseBasicParsing | Out-Null; return $true } catch { return $false }
}
function Model-Ready {
    if (-not (Test-Path $modelFile)) { return $false }
    return ((Get-Item $modelFile).Length -eq $modelBytes)
}

# ---- requirements report
function Show-Requirements([bool]$forInstall) {
    Head 'Stable Diffusion requirements'
    if ($isWin) { Ok "Windows ($([Environment]::OSVersion.Version))" }
    elseif ($OnMac) { Ok "macOS (webui.sh$(if ($appleGpu) { ', Apple Silicon GPU' }))" }
    else { Ok 'Linux (webui.sh; needs the python3.10 venv module, e.g. apt package python3.10-venv)' }

    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) { Ok "git  $($git.Source)" } else { Bad "git is not on PATH (./sd.ps1 -Install installs it: $(Get-InstallHint $gitPkg))" }

    $py = Find-Python310
    if ($py) { Ok "$($py.Version)  $($py.Path)" }
    else { Bad "Python 3.10 is not installed (A1111 does not work with 3.11 or newer; ./sd.ps1 -Install installs 3.10 beside your other Pythons: $(Get-InstallHint $pyPkg))" }

    $gpu = Get-Gpu
    if ($gpu) {
        if ($gpu.MiB -ge 6000) { Ok "GPU  $($gpu.Name), $($gpu.MiB) MiB, driver $($gpu.Driver)" }
        elseif ($gpu.MiB -ge 4000) { Warn "GPU  $($gpu.Name) has only $($gpu.MiB) MiB; it will run with --medvram (slower)" }
        else { Warn "GPU  $($gpu.Name) has only $($gpu.MiB) MiB; it will run with --lowvram (slow)" }
    } elseif ($appleGpu) { Ok 'GPU  Apple Silicon (Metal)' }
    elseif ($Cpu) { Warn 'no NVIDIA GPU: running on the CPU (-Cpu); expect minutes per picture' }
    else { Bad 'no NVIDIA GPU found (nvidia-smi is missing). Install the NVIDIA driver, or use -Cpu to run on the CPU (very slow)' }

    $free = Get-FreeGb $sdRoot
    if ($free -lt 0) { Warn "could not read free disk space for $sdRoot" }
    elseif ($free -ge $minFreeGb) { Ok "disk  $free GB free for $sdRoot" }
    elseif (Test-Path $venvPy) { Ok "disk  $free GB free (already installed)" }
    else { Bad "disk  only $free GB free for $sdRoot; the install needs about $minFreeGb GB (PyTorch 5 GB, repository, 4 GB model). Use -SdHome on another drive" }

    $cloned = Test-Path $launcher
    $ready  = Test-Path $venvPy
    if ($ready) { Ok "AUTOMATIC1111 installed and set up  $webui" }
    elseif ($cloned) { Warn "AUTOMATIC1111 is cloned ($webui) but not set up yet; the first -Start installs PyTorch (10-20 minutes)" }
    else { Warn "AUTOMATIC1111 is not installed (./sd.ps1 -Install clones $repoUrl at $repoTag to $webui)" }

    if (Model-Ready) { Ok "model  $modelFile" }
    elseif (Test-Path $modelFile) { Warn "model file is incomplete ($((Get-Item $modelFile).Length) of $modelBytes bytes); -Install resumes the download" }
    else { Warn "model not downloaded (-Install fetches v1-5-pruned-emaonly.safetensors, 4.3 GB, into the model store at $modelDir)" }

    if (Test-Api) { Ok "API answering at $apiUrl" }
    elseif (Get-Port-Free) { Ok "port $Port is free" }
    else { Bad "port $Port is in use by something that is not an A1111 API; pick another -Port (and set the url in sim/data/images.json)" }

    if ($forInstall -and -not ($cloned -and (Model-Ready))) {
        foreach ($t in @(@('github.com', 'https://github.com'), @('huggingface.co', 'https://huggingface.co'), @('pypi.org', 'https://pypi.org'), @('download.pytorch.org', 'https://download.pytorch.org'))) {
            if (Test-Url $t[1]) { Ok "network  $($t[0]) reachable" } else { Warn "network  $($t[0]) is not reachable (needed for the install)" }
        }
    }
    Write-Host ''
    Write-Host 'Model licence: Stable Diffusion 1.5 is CreativeML OpenRAIL-M; downloading it means you accept that licence.' -ForegroundColor DarkGray
}

function Confirm-Install($what, $pkg) {
    if (-not (Get-PackageName $pkg)) { return $false }
    if ($Yes) { return $true }
    $a = ''
    try { $a = Read-Host "Install $what ($(Get-InstallHint $pkg))? [y/N]" } catch {}
    return ($a -match '^(y|yes)$')
}

# ---- install
function Install-All {
    Show-Requirements $true
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        if (Confirm-Install 'git' $gitPkg) { Install-Package $gitPkg | Out-Null }
    }
    if (-not (Find-Python310)) {
        if (Confirm-Install 'Python 3.10' $pyPkg) { Install-Package $pyPkg | Out-Null }
    }
    $script:failed = 0
    Head 'Installing'
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Bad 'git is still not on PATH (open a new terminal after installing it, then run this again)'; return }
    if (-not (Find-Python310)) {
        if ($isWin) { Bad 'Python 3.10 is still missing (install it from python.org, tick "Add to PATH", then run this again)' }
        else { Bad 'Python 3.10 is still missing (pyenv install 3.10, uv python install 3.10, or the deadsnakes PPA on Ubuntu; then run this again)' }
        return
    }
    if (-not (Get-Gpu) -and -not $Cpu -and -not $appleGpu) { Bad 'no NVIDIA GPU; rerun with -Cpu to continue anyway'; return }

    New-Item -ItemType Directory -Force -Path $sdRoot | Out-Null
    if (Test-Path $launcher) { Ok 'AUTOMATIC1111 already cloned' }
    else {
        Write-Host "  cloning $repoUrl ($repoTag) ..."
        git clone --depth 1 --branch $repoTag $repoUrl $webui
        if ($LASTEXITCODE -or -not (Test-Path $launcher)) { Bad "git clone failed (see above)"; return }
        Ok "cloned to $webui"
    }

    if (Model-Ready) { Ok 'model already in the model store' }
    elseif (-not (Get-ModelFile $modelFile $modelUrl $modelBytes 'the model (4.3 GB)')) { return }
    Write-Host ''
    Write-Host 'Installed. Next: ./sd.ps1 -Start   (the first start installs PyTorch and takes 10-20 minutes)' -ForegroundColor Green
}

# Download one file into the model store with curl (resumes an interrupted download). $true when the file is complete.
function Get-ModelFile($file, $url, $bytes, $what) {
    if ((Test-Path $file) -and ((Get-Item $file).Length -eq $bytes)) { return $true }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null
    $curl = if ($isWin) { 'curl.exe' } else { 'curl' }
    if (-not (Get-Command $curl -ErrorAction SilentlyContinue)) { Bad "$curl is not available to download $what; download $url to $file yourself"; return $false }
    $part = "$file.part"
    Write-Host "  downloading $what (resumes if interrupted) ..."
    & $curl -L --fail --retry 5 --retry-delay 5 -C - -o $part $url
    if ($LASTEXITCODE) { Bad "download failed (curl exit $LASTEXITCODE); run this again to resume"; return $false }
    if ((Get-Item $part).Length -ne $bytes) { Bad "downloaded file is $((Get-Item $part).Length) bytes, expected $bytes; delete $part and try again"; return $false }
    Move-Item -Force $part $file
    Ok "saved $file"
    return $true
}
function Confirm-Download($question) {
    if ($Yes) { return $true }
    $a = ''
    try { $a = Read-Host "$question [y/N]" } catch {}
    return ($a -match '^(y|yes)$')
}

# Mature checkpoints this script knows how to fetch, by model-store folder name (sim/data/images.json "mature"."checkpoint").
# Each is an SD 1.5 fine-tune that can draw nudity and runs comfortably on an 8 GB card. Any other folder you fill yourself.
$knownMature = @{
    'realistic-vision-5.1' = @{
        What  = 'Realistic Vision 5.1 (SD 1.5, fp16, 2.1 GB, CreativeML OpenRAIL-M)'
        Url   = 'https://huggingface.co/SG161222/Realistic_Vision_V5.1_noVAE/resolve/main/Realistic_Vision_V5.1_fp16-no-ema.safetensors'
        File  = 'Realistic_Vision_V5.1_fp16-no-ema.safetensors'
        Bytes = 2132625894
        Vae   = 'sd-vae-ft-mse'   # the model card asks for this VAE (sharper, no washed-out colours)
    }
}
$knownVae = @{
    'sd-vae-ft-mse' = @{
        What  = 'the sd-vae-ft-mse VAE (330 MB)'
        Url   = 'https://huggingface.co/stabilityai/sd-vae-ft-mse-original/resolve/main/vae-ft-mse-840000-ema-pruned.safetensors'
        File  = 'vae-ft-mse-840000-ema-pruned.safetensors'
        Bytes = 334641190
    }
}

# webui.bat ends with "pause": once the server process has ended (it never does on success) the hidden window waits for a key forever.
function Test-LaunchEnded {
    foreach ($f in @($errFile, $logFile)) {
        if ((Test-Path $f) -and (Get-Content $f -Tail 3 -ErrorAction SilentlyContinue | Where-Object { $_ -match 'Press any key to continue' })) { return $true }
    }
    return $false
}
function Stop-StuckLaunch($proc) {
    if ($proc -and -not $proc.HasExited) { Stop-Tree $proc.Id }
}

# ---- start / stop
function Start-Sd {
    if (Test-Api) { Ok "already running at $apiUrl"; return }
    Show-Requirements $false
    if ($script:failed) { Write-Host "`nNot starting: fix the failed requirements above." -ForegroundColor Red; return }
    if (-not (Test-Path $launcher)) { Bad 'AUTOMATIC1111 is not installed; run ./sd.ps1 -Install first'; return }
    if (-not (Model-Ready)) { Bad 'the model is not in the model store; run ./sd.ps1 -Install first'; return }

    $py = Find-Python310
    $args_ = @('--api', '--nowebui', '--port', "$Port", '--ckpt-dir', ('"' + $modelDir.Replace('\', '/') + '"'), '--no-download-sd-model')
    $gpu = Get-Gpu
    if ($appleGpu -and -not $Cpu) { }   # webui.sh adds the macOS flags itself (webui-macos-env.sh)
    elseif ($Cpu -or -not $gpu) { $args_ += @('--use-cpu', 'all', '--no-half', '--skip-torch-cuda-test') }
    elseif ($gpu.MiB -lt 4000) { $args_ += '--lowvram' }
    elseif ($gpu.MiB -lt 9000 -and $Tune -in 'auto', 'medvram') { $args_ += '--medvram' }   # 8 GB cards run out of memory at 768 px wide pictures without it
    if ($Mature) {
        $name = $MatureModel
        if (-not $name) {
            $ij = PathOf $PSScriptRoot 'sim' 'data' 'images.json'
            try { $j = Read-JsonFile $ij; if ($j.mature -and $j.mature.checkpoint) { $name = "$($j.mature.checkpoint)" } } catch {}
        }
        if (-not $name) { Warn 'mature: no checkpoint named (-MatureModel, or "mature": {"checkpoint": ...} in sim/data/images.json); using sd15' }
        else {
            $dir = PathOf $modelHome $name
            $find = { if (Test-Path $dir) { Get-ChildItem -Path $dir -Recurse -File -Include '*.safetensors', '*.ckpt' -ErrorAction SilentlyContinue | Select-Object -First 1 } else { $null } }
            $w = & $find
            $k = $knownMature[$name]
            if (-not $w -and $k) {
                if (Confirm-Download "mature: download $($k.What) into $dir?") {
                    if (Get-ModelFile (PathOf $dir $k.File) $k.Url $k.Bytes $k.What) { $w = & $find }
                    else { $script:failed = 0 }   # not fatal either: the base model draws (run this again to resume the download)
                } else { Info "not downloading; ./web.ps1 -Mature (or ./sd.ps1 -Start -Mature) offers again" }
            }
            if (-not $w) {
                # not fatal: the base model draws, and the game still adds the adults-only words and negative prompt
                Warn "mature: no .safetensors or .ckpt in $dir, so the base model $ModelName draws$(if (-not $k) { ' (put the checkpoint file in that folder of the model store)' })"
            } else {
                $args_ += @('--ckpt', ('"' + $w.FullName.Replace('\', '/') + '"'))
                Ok "mature checkpoint  $($w.FullName)"
                if ($k -and $k.Vae) {
                    $v = $knownVae[$k.Vae]
                    $vaeFile = PathOf $modelHome $k.Vae $v.File
                    if ((Test-Path $vaeFile) -or (Confirm-Download "mature: download $($v.What), which $name is meant to be used with?")) {
                        if (Get-ModelFile $vaeFile $v.Url $v.Bytes $v.What) { $args_ += @('--vae-path', ('"' + $vaeFile.Replace('\', '/') + '"')); Ok "VAE  $vaeFile" }
                        else { $script:failed = 0 }   # a missing VAE only costs some colour; carry on without it
                    }
                }
            }
        }
    }
    if ($env:DND_SD_ARGS) { $args_ += $env:DND_SD_ARGS }

    # A previous start that hung (webui.bat waits for a key after an error) would still be holding the pid file.
    if (Test-Path $pidFile) {
        $old = [int](Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($old -and (Get-Process -Id $old -ErrorAction SilentlyContinue)) { Stop-Tree $old }
        Remove-Item -Force $pidFile -ErrorAction SilentlyContinue
    }
    $saved = @{ CA = $env:COMMANDLINE_ARGS; PY = $env:PYTHON; PYC = $env:python_cmd; ROOT = $env:can_run_as_root; ALLOC = $env:PYTORCH_CUDA_ALLOC_CONF }
    if (-not $env:PYTORCH_CUDA_ALLOC_CONF -and $Tune -eq 'alloc') { $env:PYTORCH_CUDA_ALLOC_CONF = 'garbage_collection_threshold:0.8,max_split_size_mb:128' }   # opt-in only: with this the server process exits silently after loading on an RTX 2070 (torch 2.1.2, Windows)
    $env:COMMANDLINE_ARGS = ($args_ -join ' ')
    if ($py) { $env:PYTHON = $py.Path; $env:python_cmd = $py.Path }   # webui.bat reads PYTHON, webui.sh reads python_cmd
    $env:can_run_as_root = '1'
    New-Item -ItemType Directory -Force -Path $sdRoot | Out-Null
    Write-Host "  starting: $launcher"
    Write-Host "  flags:    $($env:COMMANDLINE_ARGS)"
    try {
        if ($isWin) {
            $proc = Start-Process -FilePath 'cmd.exe' -ArgumentList @('/c', 'webui.bat') -WorkingDirectory $webui -WindowStyle Hidden -RedirectStandardOutput $logFile -RedirectStandardError $errFile -PassThru
        } else {
            $proc = Start-Process -FilePath 'bash' -ArgumentList @('webui.sh') -WorkingDirectory $webui -RedirectStandardOutput $logFile -RedirectStandardError $errFile -PassThru
        }
    } finally {
        $env:COMMANDLINE_ARGS = $saved.CA; $env:PYTHON = $saved.PY; $env:python_cmd = $saved.PYC; $env:can_run_as_root = $saved.ROOT; $env:PYTORCH_CUDA_ALLOC_CONF = $saved.ALLOC
    }
    if (-not $proc) { Bad 'could not start the server'; return }
    Set-Content -Path $pidFile -Value $proc.Id
    if ($NoWait) {
        for ($i = 0; $i -lt 20 -and -not $proc.HasExited -and -not (Test-LaunchEnded); $i++) { Start-Sleep -Seconds 1 }
        if ($proc.HasExited -or (Test-LaunchEnded)) {
            Stop-StuckLaunch $proc
            Bad "the server exited with code $($proc.ExitCode) right after starting. Last log lines:"
            foreach ($f in @($errFile, $logFile)) { if (Test-Path $f) { Get-Content $f -Tail 8 | ForEach-Object { Info $_ } } }
            Remove-Item -Force $pidFile -ErrorAction SilentlyContinue
            return
        }
        Ok "launched in the background (log: $logFile); the Pictures panel works once it answers at $apiUrl"; return
    }
    if (-not (Test-Path $venvPy)) { Write-Host '  first run: installing PyTorch and the web UI requirements (10-20 minutes)' -ForegroundColor Yellow }

    $deadline = (Get-Date).AddMinutes($TimeoutMin)
    $tick = 0
    while ((Get-Date) -lt $deadline) {
        if (Test-Api) { Ok "API answering at $apiUrl"; Write-Host 'Stable Diffusion is running. Log: ' $logFile -ForegroundColor Green; return }
        if ($proc.HasExited -or (Test-LaunchEnded)) {
            Stop-StuckLaunch $proc
            Bad "the server exited with code $($proc.ExitCode). Last log lines:"
            foreach ($f in @($errFile, $logFile)) { if (Test-Path $f) { Get-Content $f -Tail 8 | ForEach-Object { Info $_ } } }
            Remove-Item -Force $pidFile -ErrorAction SilentlyContinue
            return
        }
        Start-Sleep -Seconds 5
        $tick++
        if ($tick % 6 -eq 0) {
            $last = ''
            foreach ($f in @($errFile, $logFile)) { if (Test-Path $f) { $l = (Get-Content $f -Tail 1 -ErrorAction SilentlyContinue); if ($l) { $last = "$l"; break } } }
            if ($last.Length -gt 110) { $last = $last.Substring(0, 110) }
            Info "still starting ... $last"
        }
    }
    Bad "the API did not answer within $TimeoutMin minutes (see $logFile and $errFile)"
}

function Stop-Sd {
    if (-not (Test-Path $pidFile)) {
        if (Test-Api) { Warn "an API is answering at $apiUrl but this script did not start it, so it is left running" } else { Ok 'nothing to stop' }
        return
    }
    $id = [int](Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    Stop-Tree $id
    Remove-Item -Force $pidFile -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    if (Test-Api) { Warn "an API still answers at $apiUrl (another process owns it)" } else { Ok 'stopped' }
}

if ($Installed) {
    $ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction = $cnfSaved
    if ((Test-Path $launcher) -and (Model-Ready)) { exit 0 } else { exit 1 }
}
if ($Install) { Install-All }
if ($Check -and -not $Install) { Show-Requirements $true }
if ($Start) { $script:failed = 0; Start-Sd }
if ($Stop) { Stop-Sd }

Write-Host ''
$ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction = $cnfSaved
if ($script:failed) { Write-Host "$($script:failed) problem(s), $($script:warned) warning(s)." -ForegroundColor Red; exit 1 }
Write-Host "Done. $($script:warned) warning(s)." -ForegroundColor Green
exit 0
