# Stable Diffusion for CppDnD: check what it needs, install it, start it, stop it.
# CppDnD draws pictures through an AUTOMATIC1111 server (started with --api). This script sets one up.
#   .\sd.ps1                 same as -Check
#   .\sd.ps1 -Check          report every requirement (changes nothing)
#   .\sd.ps1 -Install        install what is missing: git and Python 3.10 (via winget, asks first),
#                            AUTOMATIC1111 (cloned), and the SD 1.5 model (into the CppLmmModelStore)
#   .\sd.ps1 -Start          start the API server in the background and wait until it answers
#                            (the first start installs PyTorch, about 5 GB, and takes 10-20 minutes)
#   .\sd.ps1 -Stop           stop a server this script started
#   -Installed               print nothing; exit 0 if AUTOMATIC1111 and the model are installed, 1 if not (web.ps1 uses this)
#   -NoWait                  with -Start: return as soon as the server is launched (web.ps1 uses this)
#   -Yes                     do not ask before installing with winget
#   -Port 7860               API port (must match "url" in sim\data\images.json)
#   -SdHome DIR              where AUTOMATIC1111 lives (default %LOCALAPPDATA%\CppDnD\sd, or $env:DND_SD_HOME)
#   -ModelName sd15          the model-store folder that holds the checkpoint (matches "checkpoint" in images.json)
#   -Cpu                     no NVIDIA GPU: run on the CPU (works, but a picture takes minutes)
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

$isWin = ($env:OS -eq 'Windows_NT')
if (-not ($Check -or $Install -or $Start -or $Stop -or $Installed)) { $Check = $true }

# A profile may install a command-not-found handler that prints a line for every Get-Command probe below; mute it while this script runs.
$cnfSaved = $ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction
$ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction = $null

# ---- where things live
$sdRoot = $SdHome
if (-not $sdRoot) { $sdRoot = $env:DND_SD_HOME }
if (-not $sdRoot) {
    if ($isWin) { $sdRoot = Join-Path $env:LOCALAPPDATA 'CppDnD\sd' } else { $sdRoot = Join-Path $env:HOME '.local/share/cppdnd/sd' }
}
$webui    = Join-Path $sdRoot 'stable-diffusion-webui'
$launcher = if ($isWin) { Join-Path $webui 'webui.bat' } else { Join-Path $webui 'webui.sh' }
$venvPy   = if ($isWin) { Join-Path $webui 'venv\Scripts\python.exe' } else { Join-Path $webui 'venv/bin/python' }
$logFile  = Join-Path $sdRoot 'sd.log'
$errFile  = Join-Path $sdRoot 'sd.err.log'
$pidFile  = Join-Path $sdRoot 'sd.pid'
$repoUrl  = if ($env:DND_SD_REPO) { $env:DND_SD_REPO } else { 'https://github.com/AUTOMATIC1111/stable-diffusion-webui' }
$repoTag  = if ($env:DND_SD_TAG)  { $env:DND_SD_TAG }  else { 'v1.10.1' }
$apiUrl   = "http://127.0.0.1:$Port"

$modelHome = $env:DEEPSEEK_MODEL_HOME
if (-not $modelHome) {
    if ($isWin) { $modelHome = Join-Path $env:USERPROFILE '.local\share\deepseek\models' } else { $modelHome = Join-Path $env:HOME '.local/share/deepseek/models' }
}
$modelDir   = Join-Path $modelHome $ModelName
$modelFile  = Join-Path $modelDir 'v1-5-pruned-emaonly.safetensors'
$modelUrl   = 'https://huggingface.co/stable-diffusion-v1-5/stable-diffusion-v1-5/resolve/main/v1-5-pruned-emaonly.safetensors'
$modelBytes = 4265146304
$minFreeGb  = 15

# ---- probes
function Test-Api {
    try { Invoke-RestMethod -Uri "$apiUrl/sdapi/v1/options" -TimeoutSec 3 | Out-Null; return $true } catch { return $false }
}
function Get-Port-Free {
    try {
        $l = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
        $l.Start(); $l.Stop(); return $true
    } catch { return $false }
}
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
function Get-Gpu {
    $smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
    if (-not $smi) { return $null }
    $line = (& nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader,nounits 2>$null | Select-Object -First 1)
    if (-not $line) { return $null }
    $f = "$line".Split(',') | ForEach-Object { $_.Trim() }
    if ($f.Count -lt 3) { return $null }
    return @{ Name = $f[0]; MiB = [int]$f[1]; Driver = $f[2] }
}
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
function Refresh-Path {
    if ($isWin) { $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User') }
}
function Model-Ready {
    if (-not (Test-Path $modelFile)) { return $false }
    return ((Get-Item $modelFile).Length -eq $modelBytes)
}

# ---- requirements report
function Show-Requirements([bool]$forInstall) {
    Head 'Stable Diffusion requirements'
    if ($isWin) { Ok "Windows ($([Environment]::OSVersion.Version))" }
    else { Ok 'Linux/macOS (webui.sh); only Windows has been tried with this script' }

    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) { Ok "git  $($git.Source)" } else { Bad 'git is not on PATH (.\sd.ps1 -Install installs it with winget)' }

    $py = Find-Python310
    if ($py) { Ok "$($py.Version)  $($py.Path)" }
    else { Bad 'Python 3.10 is not installed (A1111 does not work with 3.11 or newer; .\sd.ps1 -Install installs 3.10 beside your other Pythons)' }

    $gpu = Get-Gpu
    if ($gpu) {
        if ($gpu.MiB -ge 6000) { Ok "GPU  $($gpu.Name), $($gpu.MiB) MiB, driver $($gpu.Driver)" }
        elseif ($gpu.MiB -ge 4000) { Warn "GPU  $($gpu.Name) has only $($gpu.MiB) MiB; it will run with --medvram (slower)" }
        else { Warn "GPU  $($gpu.Name) has only $($gpu.MiB) MiB; it will run with --lowvram (slow)" }
    } elseif ($Cpu) { Warn 'no NVIDIA GPU: running on the CPU (-Cpu); expect minutes per picture' }
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
    else { Warn "AUTOMATIC1111 is not installed (.\sd.ps1 -Install clones $repoUrl at $repoTag to $webui)" }

    if (Model-Ready) { Ok "model  $modelFile" }
    elseif (Test-Path $modelFile) { Warn "model file is incomplete ($((Get-Item $modelFile).Length) of $modelBytes bytes); -Install resumes the download" }
    else { Warn "model not downloaded (-Install fetches v1-5-pruned-emaonly.safetensors, 4.3 GB, into the model store at $modelDir)" }

    if (Test-Api) { Ok "API answering at $apiUrl" }
    elseif (Get-Port-Free) { Ok "port $Port is free" }
    else { Bad "port $Port is in use by something that is not an A1111 API; pick another -Port (and set the url in sim\data\images.json)" }

    if ($forInstall -and -not ($cloned -and (Model-Ready))) {
        foreach ($t in @(@('github.com', 'https://github.com'), @('huggingface.co', 'https://huggingface.co'), @('pypi.org', 'https://pypi.org'), @('download.pytorch.org', 'https://download.pytorch.org'))) {
            if (Test-Url $t[1]) { Ok "network  $($t[0]) reachable" } else { Warn "network  $($t[0]) is not reachable (needed for the install)" }
        }
    }
    Write-Host ''
    Write-Host 'Model licence: Stable Diffusion 1.5 is CreativeML OpenRAIL-M; downloading it means you accept that licence.' -ForegroundColor DarkGray
}

function Confirm-Install($what) {
    if ($Yes) { return $true }
    $a = Read-Host "Install $what with winget? [y/N]"
    return ($a -match '^(y|yes)$')
}

# ---- install
function Install-All {
    Show-Requirements $true
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        if ($isWin -and (Get-Command winget -ErrorAction SilentlyContinue) -and (Confirm-Install 'git')) {
            winget install -e --id Git.Git --silent --accept-package-agreements --accept-source-agreements
            Refresh-Path
        }
    }
    if (-not (Find-Python310)) {
        if ($isWin -and (Get-Command winget -ErrorAction SilentlyContinue) -and (Confirm-Install 'Python 3.10')) {
            winget install -e --id Python.Python.3.10 --silent --accept-package-agreements --accept-source-agreements
            Refresh-Path
        }
    }
    $script:failed = 0
    Head 'Installing'
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Bad 'git is still not on PATH (open a new terminal after installing it, then run this again)'; return }
    if (-not (Find-Python310)) { Bad 'Python 3.10 is still missing (install it from python.org, tick "Add to PATH", then run this again)'; return }
    if (-not (Get-Gpu) -and -not $Cpu) { Bad 'no NVIDIA GPU; rerun with -Cpu to continue anyway'; return }

    New-Item -ItemType Directory -Force -Path $sdRoot | Out-Null
    if (Test-Path $launcher) { Ok 'AUTOMATIC1111 already cloned' }
    else {
        Write-Host "  cloning $repoUrl ($repoTag) ..."
        git clone --depth 1 --branch $repoTag $repoUrl $webui
        if ($LASTEXITCODE -or -not (Test-Path $launcher)) { Bad "git clone failed (see above)"; return }
        Ok "cloned to $webui"
    }

    if (Model-Ready) { Ok 'model already in the model store' }
    else {
        New-Item -ItemType Directory -Force -Path $modelDir | Out-Null
        $curl = if ($isWin) { 'curl.exe' } else { 'curl' }
        if (-not (Get-Command $curl -ErrorAction SilentlyContinue)) { Bad "$curl is not available to download the model; download $modelUrl to $modelFile yourself"; return }
        $part = "$modelFile.part"
        Write-Host '  downloading the model (4.3 GB, resumes if interrupted) ...'
        & $curl -L --fail --retry 5 --retry-delay 5 -C - -o $part $modelUrl
        if ($LASTEXITCODE) { Bad "download failed (curl exit $LASTEXITCODE); run -Install again to resume"; return }
        if ((Get-Item $part).Length -ne $modelBytes) { Bad "downloaded file is $((Get-Item $part).Length) bytes, expected $modelBytes; delete $part and try again"; return }
        Move-Item -Force $part $modelFile
        Ok "model saved to $modelFile"
    }
    Write-Host ''
    Write-Host 'Installed. Next: .\sd.ps1 -Start   (the first start installs PyTorch and takes 10-20 minutes)' -ForegroundColor Green
}

# webui.bat ends with "pause": once the server process has ended (it never does on success) the hidden window waits for a key forever.
function Test-LaunchEnded {
    foreach ($f in @($errFile, $logFile)) {
        if ((Test-Path $f) -and (Get-Content $f -Tail 3 -ErrorAction SilentlyContinue | Where-Object { $_ -match 'Press any key to continue' })) { return $true }
    }
    return $false
}
function Stop-StuckLaunch($proc) {
    if ($isWin -and $proc -and -not $proc.HasExited) { & taskkill.exe /PID $proc.Id /T /F 2>&1 | Out-Null }
}

# ---- start / stop
function Start-Sd {
    if (Test-Api) { Ok "already running at $apiUrl"; return }
    Show-Requirements $false
    if ($script:failed) { Write-Host "`nNot starting: fix the failed requirements above." -ForegroundColor Red; return }
    if (-not (Test-Path $launcher)) { Bad 'AUTOMATIC1111 is not installed; run .\sd.ps1 -Install first'; return }
    if (-not (Model-Ready)) { Bad 'the model is not in the model store; run .\sd.ps1 -Install first'; return }

    $py = Find-Python310
    $args_ = @('--api', '--nowebui', '--port', "$Port", '--ckpt-dir', ('"' + $modelDir.Replace('\', '/') + '"'), '--no-download-sd-model')
    $gpu = Get-Gpu
    if ($Cpu -or -not $gpu) { $args_ += @('--use-cpu', 'all', '--no-half', '--skip-torch-cuda-test') }
    elseif ($gpu.MiB -lt 4000) { $args_ += '--lowvram' }
    elseif ($gpu.MiB -lt 9000) { $args_ += '--medvram' }   # 8 GB cards run out of memory at 768 px wide pictures without it
    if ($env:DND_SD_ARGS) { $args_ += $env:DND_SD_ARGS }

    # A previous start that hung (webui.bat waits for a key after an error) would still be holding the pid file.
    if (Test-Path $pidFile) {
        $old = [int](Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($old -and (Get-Process -Id $old -ErrorAction SilentlyContinue)) { if ($isWin) { & taskkill.exe /PID $old /T /F 2>&1 | Out-Null } else { & kill $old 2>&1 | Out-Null } }
        Remove-Item -Force $pidFile -ErrorAction SilentlyContinue
    }
    $saved = @{ CA = $env:COMMANDLINE_ARGS; PY = $env:PYTHON; ROOT = $env:can_run_as_root; ALLOC = $env:PYTORCH_CUDA_ALLOC_CONF }
    if (-not $env:PYTORCH_CUDA_ALLOC_CONF) { $env:PYTORCH_CUDA_ALLOC_CONF = 'garbage_collection_threshold:0.8,max_split_size_mb:128' }   # less fragmentation: "out of memory" with gigabytes still free
    $env:COMMANDLINE_ARGS = ($args_ -join ' ')
    if ($py) { $env:PYTHON = $py.Path }
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
        $env:COMMANDLINE_ARGS = $saved.CA; $env:PYTHON = $saved.PY; $env:can_run_as_root = $saved.ROOT; $env:PYTORCH_CUDA_ALLOC_CONF = $saved.ALLOC
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
    if ($isWin) { & taskkill.exe /PID $id /T /F 2>&1 | Out-Null } else { & kill $id 2>&1 | Out-Null }
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
