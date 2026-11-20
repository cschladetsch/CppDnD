# Set up CppDnD on a fresh Windows machine: tools, source, build, tests, and (optionally) the local AI and picture features.
#   .\install.ps1                      everything, asking before each install or download
#   .\install.ps1 -Yes                 do not ask (installs Ollama, its models and Stable Diffusion too: about 25 GB)
#   .\install.ps1 -Minimal             the game only: no Ollama, no Stable Diffusion
#   .\install.ps1 -NoLlm               skip Ollama and its models
#   .\install.ps1 -NoArt               skip Stable Diffusion (picture generation, needs an NVIDIA GPU)
#   .\install.ps1 -NoTests             build, but do not run the test suite
#   .\install.ps1 -Dir D:\games\CppDnD where to put the source when this script is run on its own (default: .\CppDnD)
#   .\install.ps1 -Update              git pull an existing checkout first
#   .\install.ps1 -Rebuild             wipe sim\build and configure again
#   .\install.ps1 -Repo URL -Branch main
#
# Works from a downloaded copy of this one file or from inside a clone. If it is not inside a clone it clones the repository first.
# If scripts are blocked on this machine, start it with:  powershell -ExecutionPolicy Bypass -File .\install.ps1
#
# What it does, in order (each step is skipped when it is already done, so it is safe to run again):
#   1. winget                     checked (Windows 10 1809+ and 11 have it as "App Installer"; it is the one thing this script cannot install)
#   2. git, cmake, ninja          installed with winget when missing
#   3. C++ toolchain              Visual Studio Build Tools (C++ workload: the standard library and Windows SDK) and LLVM clang++
#   4. source                     cloned from GitHub unless this script already sits in a checkout
#   5. build                      cmake + Ninja + clang++ (Release), the same configuration run.ps1 uses; fetches the model store and GoogleTest
#   6. tests                      ctest
#   7. Ollama and its models      optional: the plain-English prompt and the AI table (models are read from sim\data\agents.json)
#   8. Stable Diffusion           optional: delegates to sd.ps1 -Install (NVIDIA GPU, Python 3.10, about 15 GB)
#   9. a summary of what is ready and how to start it
# Needs network access to github.com for the source and CMake's fetches (plus huggingface.co, pypi.org and download.pytorch.org for pictures,
# and ollama.com for models). Nothing is changed outside the install locations of the tools above and the checkout.
param(
    [string]$Dir = '',
    [string]$Repo = 'https://github.com/cschladetsch/CppDnD.git',
    [string]$Branch = '',
    [switch]$Yes,
    [switch]$Minimal,
    [switch]$NoLlm,
    [switch]$NoArt,
    [switch]$NoTests,
    [switch]$Update,
    [switch]$Rebuild
)
$ErrorActionPreference = 'Stop'
if ($Minimal) { $NoLlm = $true; $NoArt = $true }

# A profile may install a command-not-found handler that prints a line for every probe below; mute it while this script runs.
$cnfSaved = $ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction
$ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction = $null
function Restore-Cnf { $ExecutionContext.SessionState.InvokeCommand.CommandNotFoundAction = $cnfSaved }

$script:warnings = @()
function Head($msg) { Write-Host "`n$msg" -ForegroundColor Cyan }
function Ok($msg)   { Write-Host "  [ ok ] $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "  [warn] $msg" -ForegroundColor Yellow; $script:warnings += $msg }
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
function Winget-Install($id, $extra = @()) {
    $a = @('install', '-e', '--id', $id, '--silent', '--accept-package-agreements', '--accept-source-agreements') + $extra
    & winget @a
    # 0x8A150061 (already installed) and "no applicable upgrade" are fine; anything else is checked by the caller looking for the tool
    Refresh-Path
}

# ---- 0. this machine
Head 'This machine'
$onWindows = ($PSVersionTable.PSVersion.Major -le 5) -or ($IsWindows -eq $true)
if (-not $onWindows) { Fail 'install.ps1 sets up Windows. On Linux or macOS install git, cmake, ninja and a C++20 compiler with your package manager, then: cmake -S sim -B sim/build -G Ninja && cmake --build sim/build' }
if (-not [Environment]::Is64BitOperatingSystem) { Fail '64-bit Windows is required' }
Ok "Windows $([Environment]::OSVersion.Version), PowerShell $($PSVersionTable.PSVersion)"
if ($PSVersionTable.PSVersion.Major -le 5) {
    # Windows PowerShell 5.1 needs TLS 1.2 for GitHub
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
}
foreach ($f in Get-ChildItem -Path $PSScriptRoot -Filter *.ps1 -ErrorAction SilentlyContinue) { try { Unblock-File -Path $f.FullName } catch {} }

# ---- 1. winget
Head 'winget'
if (Have 'winget') { Ok "winget  $((Get-Command winget).Source)" }
else {
    Warn 'winget is not installed'
    Fail 'Install "App Installer" from the Microsoft Store (https://aka.ms/getwinget), or update Windows, open a new terminal and run this again.'
}

# ---- 2. git, cmake, ninja
Head 'git, cmake, ninja'
$tools = @(
    @{ Name = 'git';   Id = 'Git.Git';            Dir = (Join-Path $env:ProgramFiles 'Git\cmd') },
    @{ Name = 'cmake'; Id = 'Kitware.CMake';      Dir = (Join-Path $env:ProgramFiles 'CMake\bin') },
    @{ Name = 'ninja'; Id = 'Ninja-build.Ninja';  Dir = '' }
)
foreach ($t in $tools) {
    if (-not (Have $t.Name)) { Add-PathIfThere $t.Dir }
    if (-not (Have $t.Name)) {
        if (Ask "$($t.Name) is not installed. Install it with winget ($($t.Id))?") {
            Winget-Install $t.Id
            Add-PathIfThere $t.Dir
        }
    }
    if (-not (Have $t.Name) -and $t.Name -eq 'ninja') {  # winget puts it under the package folder and may need a new shell to see it
        $pkg = Get-ChildItem -Path (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages') -Filter 'Ninja-build.Ninja*' -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($pkg) { Add-PathIfThere $pkg.FullName }
    }
    if (Have $t.Name) { Ok "$($t.Name)  $((Get-Command $t.Name).Source)" }
    else { Fail "'$($t.Name)' is needed. Install it (winget install $($t.Id)), open a new terminal and run this again." }
}

# ---- 3. C++ toolchain: clang++ compiles, but on Windows it uses the Visual Studio C++ standard library and the Windows SDK
Head 'C++ toolchain'
function Find-VsCpp {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }
    $p = & $vswhere -latest -products '*' -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' -property installationPath 2>$null
    if ($p) { return ($p | Select-Object -First 1) }
    return $null
}
$vs = Find-VsCpp
if ($vs) { Ok "Visual Studio C++ tools  $vs" }
elseif (Ask 'The Visual Studio C++ build tools (standard library and Windows SDK, about 3-6 GB, needs administrator approval) are not installed. Install them?') {
    Info 'installing Visual Studio Build Tools 2022 with the C++ workload; this takes several minutes and shows a UAC prompt ...'
    Winget-Install 'Microsoft.VisualStudio.2022.BuildTools' @('--override', '--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended')
    $vs = Find-VsCpp
    if ($vs) { Ok "Visual Studio C++ tools  $vs" }
    else { Fail 'the C++ build tools did not install (a restart may be pending). Restart Windows, then run this again.' }
}
else { Fail 'The C++ build tools are required to compile the game.' }

if (-not (Have 'clang++')) { Add-PathIfThere (Join-Path $env:ProgramFiles 'LLVM\bin') }
if (-not (Have 'clang++')) {
    if (Ask 'clang++ is not installed. Install LLVM with winget (LLVM.LLVM)?') {
        Winget-Install 'LLVM.LLVM'
        Add-PathIfThere (Join-Path $env:ProgramFiles 'LLVM\bin')
    }
}
if (Have 'clang++') { Ok "clang++  $((Get-Command clang++).Source)" }
else { Fail "'clang++' is needed. Install it (winget install LLVM.LLVM), open a new terminal and run this again." }

# does the compiler really work with the C++20 library? (this catches a toolchain that is installed but not usable)
$tmp = Join-Path ([IO.Path]::GetTempPath()) ('cppdnd_probe_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    Set-Content -Path (Join-Path $tmp 'p.cpp') -Value "#include <vector>`n#include <string>`n#include <ranges>`nint main() { std::vector<int> v{1,2,3}; return static_cast<int>(v.size()) - 3; }"
    Push-Location $tmp
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'  # Windows PowerShell 5.1 turns native stderr into errors
    try { & clang++ -std=c++20 p.cpp -o p.exe 2>&1 | Out-Null; $built = ($LASTEXITCODE -eq 0) -and (Test-Path 'p.exe') } finally { $ErrorActionPreference = $eap; Pop-Location }
    if ($built) { Ok 'clang++ compiles C++20' }
    else { Fail 'clang++ cannot compile a C++20 program. Check that the Visual Studio C++ workload and a Windows SDK are installed (run the Visual Studio Installer, choose Modify, "Desktop development with C++").' }
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }

# ---- 4. the source
Head 'Source'
$root = $null
if (Test-Path (Join-Path $PSScriptRoot 'sim\CMakeLists.txt')) { $root = $PSScriptRoot }
elseif ($Dir -and (Test-Path (Join-Path $Dir 'sim\CMakeLists.txt'))) { $root = (Resolve-Path $Dir).Path }
elseif (-not $Dir -and (Test-Path (Join-Path (Get-Location).Path 'CppDnD\sim\CMakeLists.txt'))) { $root = (Resolve-Path '.\CppDnD').Path }
if (-not $root) {
    $target = if ($Dir) { $Dir } else { Join-Path (Get-Location).Path 'CppDnD' }
    if ((Test-Path $target) -and (Get-ChildItem $target -Force -ErrorAction SilentlyContinue | Select-Object -First 1)) { Fail "$target exists and is not a CppDnD checkout; pick another folder with -Dir" }
    Info "cloning $Repo into $target ..."
    $cloneArgs = @('clone', '--depth', '1')
    if ($Branch) { $cloneArgs += @('--branch', $Branch) }
    git @cloneArgs $Repo $target
    if ($LASTEXITCODE -or -not (Test-Path (Join-Path $target 'sim\CMakeLists.txt'))) { Fail 'git clone failed (see above). Check the network and that github.com is reachable.' }
    $root = (Resolve-Path $target).Path
    Ok "cloned to $root"
} else {
    Ok "using the checkout at $root"
    if ($Update) {
        if (Test-Path (Join-Path $root '.git')) {
            Info 'git pull ...'
            git -C $root pull --ff-only
            if ($LASTEXITCODE) { Warn 'git pull did not finish cleanly (local changes?); building what is there' }
        } else { Warn '-Update was given but this folder is not a git checkout' }
    }
}
$src = Join-Path $root 'sim'
$build = Join-Path $src 'build'
foreach ($f in Get-ChildItem -Path $root -Filter *.ps1 -ErrorAction SilentlyContinue) { try { Unblock-File -Path $f.FullName } catch {} }

# ---- 5. build
Head 'Build'
if ($Rebuild -and (Test-Path $build)) { Info 'removing sim\build'; Remove-Item -Recurse -Force $build }
if ((Test-Path (Join-Path $build 'CMakeCache.txt')) -and -not (Test-Path (Join-Path $build 'build.ninja'))) {
    Info 'the build folder was configured with another generator; starting it again'
    Remove-Item -Recurse -Force $build
}
if (-not (Test-Path (Join-Path $build 'build.ninja'))) {
    Info 'configuring (the first run fetches the model store and GoogleTest from GitHub) ...'
    cmake -S $src -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
    if ($LASTEXITCODE) { Fail 'cmake could not configure the project (see above). The usual causes are no network access to github.com or a toolchain that is not usable.' }
}
Info 'compiling ...'
cmake --build $build
if ($LASTEXITCODE) { Fail 'the build failed (see above)' }
$server = Join-Path $build 'dnd_server.exe'
$sim = Join-Path $build 'dnd_sim.exe'
foreach ($e in @($sim, $server)) { if (-not (Test-Path $e)) { Fail "$e was not produced" } }
Ok 'built dnd_sim.exe and dnd_server.exe'

# ---- 6. tests
if ($NoTests) { Head 'Tests'; Info 'skipped (-NoTests)' }
else {
    Head 'Tests'
    ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE) { Warn 'some tests failed (see above); the game is built and will run' } else { Ok 'every test passed' }
}

# ---- 7. Ollama and its models
$llmReady = $false
Head 'Ollama (plain-English prompt and the AI table)'
if ($NoLlm) { Info 'skipped (-NoLlm / -Minimal)' }
else {
    $ollamaUrl = 'http://127.0.0.1:11434'
    function Get-OllamaTags { try { return (Invoke-RestMethod -Uri "$ollamaUrl/api/tags" -TimeoutSec 3) } catch { return $null } }
    Add-PathIfThere (Join-Path $env:LOCALAPPDATA 'Programs\Ollama')
    if (-not (Have 'ollama')) {
        if (Ask 'Ollama is not installed. Install it with winget (Ollama.Ollama)?') {
            Winget-Install 'Ollama.Ollama'
            Add-PathIfThere (Join-Path $env:LOCALAPPDATA 'Programs\Ollama')
        }
    }
    if (-not (Have 'ollama')) { Warn 'Ollama is not installed, so the plain-English prompt and the AI table are off (rerun this later, or install it from https://ollama.com)' }
    else {
        Ok "ollama  $((Get-Command ollama).Source)"
        $tags = Get-OllamaTags
        $started = $null
        if (-not $tags) {
            Info 'starting "ollama serve" for the model downloads ...'
            $started = Start-Process -FilePath (Get-Command 'ollama').Source -ArgumentList 'serve' -WindowStyle Hidden -PassThru
            for ($i = 0; $i -lt 30 -and -not $tags; $i++) { Start-Sleep -Seconds 1; $tags = Get-OllamaTags }
        }
        if (-not $tags) { Warn 'Ollama did not answer; models were not checked (start Ollama and run this again)' }
        else {
            $wanted = @()
            try {
                $agents = Read-JsonFile (Join-Path $src 'data\agents.json')
                $walk = $null
                $walk = { param($o) if ($null -eq $o) { return }
                          if ($o -is [string]) { return }
                          if ($o -is [System.Collections.IEnumerable]) { foreach ($x in $o) { & $walk $x }; return }
                          foreach ($p in $o.PSObject.Properties) { if ($p.Name -eq 'model' -and $p.Value -is [string] -and $p.Value) { $script:wanted += $p.Value } else { & $walk $p.Value } } }
                & $walk $agents
            } catch { Warn "could not read data\agents.json: $($_.Exception.Message)" }
            $wanted = @($wanted | Select-Object -Unique)
            $have = @($tags.models | ForEach-Object { $_.name })
            $missing = @($wanted | Where-Object { $m = $_; -not ($have | Where-Object { $_ -eq $m -or $_ -eq "${m}:latest" }) })
            if ($wanted.Count -eq 0) { Warn 'no models are named in data\agents.json' }
            elseif ($missing.Count -eq 0) { Ok "every model in agents.json is installed ($($wanted -join ', '))" ; $llmReady = $true }
            elseif (Ask "Ollama is missing $($missing.Count) model(s): $($missing -join ', '). Pull them now (several GB)?") {
                foreach ($m in $missing) {
                    Info "ollama pull $m"
                    ollama pull $m
                    if ($LASTEXITCODE) { Warn "could not pull '$m' (ollama pull $m)" }
                }
                $tags = Get-OllamaTags
                $have = @($tags.models | ForEach-Object { $_.name })
                $still = @($wanted | Where-Object { $m = $_; -not ($have | Where-Object { $_ -eq $m -or $_ -eq "${m}:latest" }) })
                if ($still.Count -eq 0) { Ok 'all models installed'; $llmReady = $true } else { Warn "still missing: $($still -join ', ')" }
            }
            else { Warn "models not pulled: $($missing -join ', ') (ollama pull <name>; web.ps1 offers again)" }
        }
        if ($started -and -not $started.HasExited) { Stop-Process -Id $started.Id -Force -ErrorAction SilentlyContinue }
    }
}

# ---- 8. Stable Diffusion
$artReady = $false
Head 'Stable Diffusion (generated pictures)'
if ($NoArt) { Info 'skipped (-NoArt / -Minimal)' }
else {
    $sd = Join-Path $root 'sd.ps1'
    $gpu = $null
    try { $gpu = (Get-CimInstance Win32_VideoController | Where-Object { $_.Name -match 'NVIDIA' } | Select-Object -First 1).Name } catch {}
    if (-not (Test-Path $sd)) { Warn 'sd.ps1 is missing; pictures are not set up' }
    elseif (-not $gpu) { Warn 'no NVIDIA GPU found, so picture generation is skipped (.\sd.ps1 -Install -Cpu works without one but takes minutes per picture)' }
    else {
        Ok "NVIDIA GPU: $gpu"
        if (Ask 'Install Stable Diffusion (AUTOMATIC1111, Python 3.10, the SD 1.5 model: about 15 GB)?') {
            $a = @('-Install'); if ($Yes) { $a += '-Yes' }
            & $sd @a
            if ($LASTEXITCODE) { Warn 'sd.ps1 -Install did not finish; run it again later (it resumes)' } else { $artReady = $true; Ok 'Stable Diffusion installed (the first start also installs PyTorch, 10-20 minutes)' }
        } else { Info 'skipped; run .\sd.ps1 -Install later' }
    }
}

# ---- 9. summary
Restore-Cnf
Head 'Done'
Ok "CppDnD is in $root"
Ok 'game built'
if ($llmReady) { Ok 'Ollama and its models are ready' } elseif (-not $NoLlm) { Info 'Ollama is not fully set up (the game still runs without the AI prompt and table)' }
if ($artReady) { Ok 'Stable Diffusion is installed' } elseif (-not $NoArt) { Info 'Stable Diffusion is not installed (the game still runs without pictures)' }
if ($script:warnings.Count) {
    Write-Host "`n$($script:warnings.Count) warning(s):" -ForegroundColor Yellow
    foreach ($w in $script:warnings) { Write-Host "  - $w" -ForegroundColor Yellow }
}
Write-Host "`nStart it:" -ForegroundColor Cyan
Write-Host "  cd `"$root`""
Write-Host '  .\web.ps1                      web client at http://127.0.0.1:8088/ (starts Ollama and Stable Diffusion if they are set up)'
Write-Host '  .\run.ps1 --module meridian    terminal client'
Write-Host '  .\check.ps1                    report what is missing'
