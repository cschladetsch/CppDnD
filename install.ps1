#!/usr/bin/env pwsh
# Set up CppDnD on a fresh machine: tools, source, build, tests, and (optionally) the local AI and picture features.
# Runs in PowerShell 7 (pwsh) on Windows, Linux and macOS, and in Windows PowerShell 5.1.
#   ./install.ps1                      everything, asking before each install or download
#   ./install.ps1 -Yes                 do not ask (installs Ollama, its models and Stable Diffusion too: about 25 GB)
#   ./install.ps1 -Minimal             the game only: no Ollama, no Stable Diffusion
#   ./install.ps1 -NoLlm               skip Ollama and its models
#   ./install.ps1 -NoArt               skip Stable Diffusion (picture generation: an NVIDIA GPU, or an Apple Silicon Mac)
#   ./install.ps1 -NoTests             build, but do not run the test suite
#   ./install.ps1 -Dir ~/games/CppDnD  where to put the source when this script is run on its own (default: ./CppDnD)
#   ./install.ps1 -Update              git pull an existing checkout first
#   ./install.ps1 -Rebuild             wipe sim/build and configure again
#   ./install.ps1 -Repo URL -Branch main
#
# Works from a downloaded copy of this one file or from inside a clone. If it is not inside a clone it clones the repository first.
# Windows, if scripts are blocked:  powershell -ExecutionPolicy Bypass -File .\install.ps1
# Linux and macOS: install PowerShell first (https://aka.ms/powershell; macOS: brew install powershell), then: pwsh ./install.ps1
#
# What it does, in order (each step is skipped when it is already done, so it is safe to run again):
#   1. package manager            winget (Windows), Homebrew (macOS), apt-get, dnf, pacman or zypper (Linux); checked, not installed
#   2. git, cmake, ninja          installed with the package manager when missing (cmake 3.21 or newer, for CMakePresets.json)
#   3. C++ toolchain              Windows: Visual Studio Build Tools (C++ workload: the standard library and Windows SDK) and LLVM clang++
#                                 Linux: g++ from the package manager (or CXX); macOS: the Xcode command line tools (clang++)
#   4. source                     cloned from GitHub unless this script already sits in a checkout
#   5. build                      cmake --preset (Ninja, Release; "clang" on Windows, "release" elsewhere), the same as run.ps1;
#                                 fetches the model store and GoogleTest
#   6. tests                      ctest
#   7. Ollama and its models      optional: the plain-English prompt and the AI table (models are read from sim/data/agents.json)
#   8. Stable Diffusion           optional: delegates to sd.ps1 -Install (NVIDIA GPU or Apple Silicon, Python 3.10, about 15 GB)
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

# This file has to work on its own (downloaded without the repository), so it carries its own copy of the few helpers
# that the other scripts take from scripts/common.ps1.
$OnWindows = ($env:OS -eq 'Windows_NT')          # $IsWindows does not exist in Windows PowerShell 5.1
$OnMac     = (-not $OnWindows) -and ($IsMacOS -eq $true)
$OnLinux   = (-not $OnWindows) -and (-not $OnMac)
$ExeSuffix = if ($OnWindows) { '.exe' } else { '' }
function PathOf { return [IO.Path]::Combine([string[]]$args) }

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
function Add-PathIfThere($dir) {
    $sep = [IO.Path]::PathSeparator
    if ($dir -and (Test-Path $dir) -and (($env:PATH -split [regex]::Escape($sep)) -notcontains $dir)) { $env:PATH = "$env:PATH$sep$dir" }
}
function Refresh-Path {
    if ($OnWindows) { $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User') }
    else { foreach ($d in '/opt/homebrew/bin', '/usr/local/bin', '/home/linuxbrew/.linuxbrew/bin', '/snap/bin', (PathOf $HOME '.local' 'bin')) { Add-PathIfThere $d } }
}
function Read-JsonFile($file) {
    return ((Get-Content $file -Raw) -replace '(?s)/\*.*?\*/', '' -replace '(?m)^\s*//.*$', '') | ConvertFrom-Json
}
# Run a native program whose progress goes to stderr (Windows PowerShell 5.1 turns that into errors under 'Stop').
function Invoke-Native([scriptblock]$b) {
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { & $b } finally { $ErrorActionPreference = $eap }
}

# ---- packages: a hashtable naming the package for each manager; a value may be an array of packages
$pm = $null
if ($OnWindows) { if (Have 'winget') { $pm = 'winget' } }
elseif ($OnMac) { Refresh-Path; if (Have 'brew') { $pm = 'brew' } }
else { foreach ($m in 'apt-get', 'dnf', 'pacman', 'zypper', 'brew') { if (Have $m) { $pm = $m; break } } }
$pmKey = if ($pm -eq 'apt-get') { 'apt' } else { $pm }
$sudo = $false
if (-not $OnWindows -and $pm -ne 'brew') { try { $sudo = ((& id -u) -ne '0') } catch { $sudo = $true } }
$script:aptUpdated = $false
function Install-Hint($pkg) {
    if (-not $pm) { return 'install it with your package manager' }
    $n = $pkg[$pmKey]
    if (-not $n) { return "there is no $pm package; see its website" }
    $n = @($n) -join ' '
    $s = if ($sudo) { 'sudo ' } else { '' }
    switch ($pm) {
        'winget'  { return "winget install -e --id $n" }
        'apt-get' { return "${s}apt-get install $n" }
        'dnf'     { return "${s}dnf install $n" }
        'pacman'  { return "${s}pacman -S $n" }
        'zypper'  { return "${s}zypper install $n" }
        'brew'    { return "brew install $n" }
    }
}
function Install-Package($pkg) {
    $n = $pkg[$pmKey]
    if (-not $pm -or -not $n) { return }
    $names = @($n)
    $pre = if ($sudo) { @('sudo') } else { @() }
    Invoke-Native {
        switch ($pm) {
            'winget' {
                $a = @('install', '-e', '--id') + $names + @('--silent', '--accept-package-agreements', '--accept-source-agreements')
                if ($pkg.wingetArgs) { $a += $pkg.wingetArgs }
                & winget @a   # 0x8A150061 (already installed) is fine; the caller checks for the tool
            }
            'apt-get' {
                if (-not $script:aptUpdated) { $c = $pre + @('apt-get', 'update'); & $c[0] @($c | Select-Object -Skip 1); $script:aptUpdated = $true }
                $c = $pre + @('apt-get', 'install', '-y') + $names; & $c[0] @($c | Select-Object -Skip 1)
            }
            'dnf'    { $c = $pre + @('dnf', 'install', '-y') + $names; & $c[0] @($c | Select-Object -Skip 1) }
            'pacman' { $c = $pre + @('pacman', '-S', '--needed', '--noconfirm') + $names; & $c[0] @($c | Select-Object -Skip 1) }
            'zypper' { $c = $pre + @('zypper', '--non-interactive', 'install') + $names; & $c[0] @($c | Select-Object -Skip 1) }
            'brew'   { & brew install @names }
        }
    }
    Refresh-Path
}

# ---- 0. this machine
Head 'This machine'
if (-not [Environment]::Is64BitOperatingSystem) { Fail 'a 64-bit operating system is required' }
$osName = if ($OnWindows) { "Windows $([Environment]::OSVersion.Version)" } elseif ($OnMac) { "macOS $((& sw_vers -productVersion 2>$null))" } else { "Linux $((& uname -r 2>$null))" }
Ok "$osName, PowerShell $($PSVersionTable.PSVersion)"
if ($PSVersionTable.PSVersion.Major -le 5) {
    # Windows PowerShell 5.1 needs TLS 1.2 for GitHub
    try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}
}
function Unblock-Scripts($dir) {
    if (-not $OnWindows -or -not $dir) { return }
    foreach ($f in Get-ChildItem -Path $dir -Filter *.ps1 -Recurse -Depth 1 -ErrorAction SilentlyContinue) { try { Unblock-File -Path $f.FullName } catch {} }
}
if ($PSVersionTable.PSVersion.Major -le 4) { Fail 'PowerShell 5.1 or newer is needed (Windows PowerShell 5.1, or PowerShell 7 from https://aka.ms/powershell)' }
Unblock-Scripts $PSScriptRoot

# ---- 1. package manager
Head 'Package manager'
if ($pm) { Ok "$pm  $((Get-Command $pm).Source)" }
elseif ($OnWindows) { Fail 'winget is not installed. Install "App Installer" from the Microsoft Store (https://aka.ms/getwinget), or update Windows, open a new terminal and run this again.' }
elseif ($OnMac) { Fail 'Homebrew is not installed. Install it from https://brew.sh, open a new terminal and run this again.' }
else { Fail 'no supported package manager (apt-get, dnf, pacman, zypper or brew) was found. Install git, cmake 3.21+, ninja and a C++20 compiler yourself, then: cd sim; cmake --preset release; cmake --build build' }
if ($sudo) { Info 'installing packages uses sudo, which may ask for your password' }

# ---- 2. git, cmake, ninja
Head 'git, cmake, ninja'
$pf = if ($OnWindows) { $env:ProgramFiles } else { '' }
$tools = @(
    @{ Name = 'git';   Dir = $(if ($pf) { PathOf $pf 'Git' 'cmd' });
       Pkg = @{ winget = 'Git.Git'; apt = 'git'; dnf = 'git'; pacman = 'git'; zypper = 'git'; brew = 'git' } },
    @{ Name = 'cmake'; Dir = $(if ($pf) { PathOf $pf 'CMake' 'bin' });
       Pkg = @{ winget = 'Kitware.CMake'; apt = 'cmake'; dnf = 'cmake'; pacman = 'cmake'; zypper = 'cmake'; brew = 'cmake' } },
    @{ Name = 'ninja'; Dir = '';
       Pkg = @{ winget = 'Ninja-build.Ninja'; apt = 'ninja-build'; dnf = 'ninja-build'; pacman = 'ninja'; zypper = 'ninja'; brew = 'ninja' } }
)
foreach ($t in $tools) {
    if (-not (Have $t.Name)) { Add-PathIfThere $t.Dir }
    if (-not (Have $t.Name)) {
        if (Ask "$($t.Name) is not installed. Install it ($(Install-Hint $t.Pkg))?") {
            Install-Package $t.Pkg
            Add-PathIfThere $t.Dir
        }
    }
    if (-not (Have $t.Name) -and $t.Name -eq 'ninja' -and $OnWindows) {  # winget puts it under the package folder and may need a new shell to see it
        $pkg = Get-ChildItem -Path (PathOf $env:LOCALAPPDATA 'Microsoft' 'WinGet' 'Packages') -Filter 'Ninja-build.Ninja*' -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($pkg) { Add-PathIfThere $pkg.FullName }
    }
    if (Have $t.Name) { Ok "$($t.Name)  $((Get-Command $t.Name).Source)" }
    else { Fail "'$($t.Name)' is needed. Install it ($(Install-Hint $t.Pkg)), open a new terminal and run this again." }
}
$cmakeVer = $null
$cmakeLine = Invoke-Native { & cmake --version 2>$null | Select-Object -First 1 }
if ("$cmakeLine" -match '(\d+)\.(\d+)') { $cmakeVer = [version]"$($Matches[1]).$($Matches[2])" }
if ($cmakeVer -and $cmakeVer -lt [version]'3.21') {
    Fail "cmake $cmakeVer is too old; 3.21 or newer is needed for CMakePresets.json (pip install --user cmake, snap install cmake --classic, or apt.kitware.com)"
}

# ---- 3. C++ toolchain
Head 'C++ toolchain'
if ($OnWindows) {
    # clang++ compiles, but on Windows it uses the Visual Studio C++ standard library and the Windows SDK
    function Find-VsCpp {
        $vswhere = PathOf ${env:ProgramFiles(x86)} 'Microsoft Visual Studio' 'Installer' 'vswhere.exe'
        if (-not (Test-Path $vswhere)) { return $null }
        $p = Invoke-Native { & $vswhere -latest -products '*' -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' -property installationPath 2>$null }
        if ($p) { return ($p | Select-Object -First 1) }
        return $null
    }
    $vs = Find-VsCpp
    if ($vs) { Ok "Visual Studio C++ tools  $vs" }
    elseif (Ask 'The Visual Studio C++ build tools (standard library and Windows SDK, about 3-6 GB, needs administrator approval) are not installed. Install them?') {
        Info 'installing Visual Studio Build Tools 2022 with the C++ workload; this takes several minutes and shows a UAC prompt ...'
        Install-Package @{ winget = 'Microsoft.VisualStudio.2022.BuildTools'; wingetArgs = @('--override', '--wait --quiet --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') }
        $vs = Find-VsCpp
        if ($vs) { Ok "Visual Studio C++ tools  $vs" }
        else { Fail 'the C++ build tools did not install (a restart may be pending). Restart Windows, then run this again.' }
    }
    else { Fail 'The C++ build tools are required to compile the game.' }
    $cxx = 'clang++'
    $cxxPkg = @{ winget = 'LLVM.LLVM' }
    Add-PathIfThere (PathOf $env:ProgramFiles 'LLVM' 'bin')
} elseif ($OnMac) {
    $cxx = 'clang++'
    $cxxPkg = @{}
    $clt = Invoke-Native { & xcode-select -p 2>$null }
    if (-not $clt) {
        Info 'the Xcode command line tools are not installed; starting their installer (a dialog opens) ...'
        Invoke-Native { & xcode-select --install 2>&1 | Out-Null }
        Fail 'finish the Xcode command line tools install in the dialog, then run this again.'
    }
    Ok "Xcode command line tools  $clt"
} else {
    $cxx = if ($env:CXX) { $env:CXX } else { 'c++' }
    $cxxPkg = @{ apt = 'g++'; dnf = 'gcc-c++'; pacman = 'gcc'; zypper = 'gcc-c++'; brew = 'gcc' }
}
if (-not (Have $cxx) -and $cxxPkg.Count) {
    if (Ask "$cxx is not installed. Install it ($(Install-Hint $cxxPkg))?") {
        Install-Package $cxxPkg
        if ($OnWindows) { Add-PathIfThere (PathOf $env:ProgramFiles 'LLVM' 'bin') }
    }
}
if (Have $cxx) { Ok "$cxx  $((Get-Command $cxx).Source)" }
else { Fail "'$cxx' is needed. Install it ($(Install-Hint $cxxPkg)), open a new terminal and run this again." }

# does the compiler really work with the C++20 library? (this catches a toolchain that is installed but not usable, or a g++ older than 10)
$tmp = PathOf ([IO.Path]::GetTempPath()) ('cppdnd_probe_' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    Set-Content -Path (PathOf $tmp 'p.cpp') -Value "#include <vector>`n#include <string>`n#include <ranges>`n#include <concepts>`nint main() { std::vector<int> v{1,2,3}; return static_cast<int>(std::ranges::distance(v)) - 3; }"
    $probeExe = "p$ExeSuffix"
    Push-Location $tmp
    try { Invoke-Native { & $cxx -std=c++20 p.cpp -o $probeExe 2>&1 | Out-Null }; $built = ($LASTEXITCODE -eq 0) -and (Test-Path $probeExe) } finally { Pop-Location }
    if ($built) { Ok "$cxx compiles C++20" }
    elseif ($OnWindows) { Fail 'clang++ cannot compile a C++20 program. Check that the Visual Studio C++ workload and a Windows SDK are installed (run the Visual Studio Installer, choose Modify, "Desktop development with C++").' }
    else { Fail "$cxx cannot compile a C++20 program (g++ 10 or clang 13 or newer is needed; set CXX to pick another compiler)." }
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }

if (-not ((Have 'python3') -or ($OnWindows -and (Have 'python')))) { Info 'Python 3 is not on PATH, so the two HTTP smoke tests are left out of ctest (optional)' }

# ---- 4. the source
Head 'Source'
$root = $null
if ($PSScriptRoot -and (Test-Path (PathOf $PSScriptRoot 'sim' 'CMakeLists.txt'))) { $root = $PSScriptRoot }
elseif ($Dir -and (Test-Path (PathOf $Dir 'sim' 'CMakeLists.txt'))) { $root = (Resolve-Path $Dir).Path }
elseif (-not $Dir -and (Test-Path (PathOf (Get-Location).Path 'CppDnD' 'sim' 'CMakeLists.txt'))) { $root = (Resolve-Path (PathOf (Get-Location).Path 'CppDnD')).Path }
if (-not $root) {
    $target = if ($Dir) { $Dir } else { PathOf (Get-Location).Path 'CppDnD' }
    if ((Test-Path $target) -and (Get-ChildItem $target -Force -ErrorAction SilentlyContinue | Select-Object -First 1)) { Fail "$target exists and is not a CppDnD checkout; pick another folder with -Dir" }
    Info "cloning $Repo into $target ..."
    $cloneArgs = @('clone', '--depth', '1')
    if ($Branch) { $cloneArgs += @('--branch', $Branch) }
    Invoke-Native { git @cloneArgs $Repo $target }
    if ($LASTEXITCODE -or -not (Test-Path (PathOf $target 'sim' 'CMakeLists.txt'))) { Fail 'git clone failed (see above). Check the network and that github.com is reachable.' }
    $root = (Resolve-Path $target).Path
    Ok "cloned to $root"
} else {
    Ok "using the checkout at $root"
    if ($Update) {
        if (Test-Path (PathOf $root '.git')) {
            Info 'git pull ...'
            Invoke-Native { git -C $root pull --ff-only }
            if ($LASTEXITCODE) { Warn 'git pull did not finish cleanly (local changes?); building what is there' }
        } else { Warn '-Update was given but this folder is not a git checkout' }
    }
}
$src = PathOf $root 'sim'
$build = PathOf $src 'build'
Unblock-Scripts $root

# ---- 5. build
Head 'Build'
if ($Rebuild -and (Test-Path $build)) { Info 'removing sim/build'; Remove-Item -Recurse -Force $build }
if ((Test-Path (PathOf $build 'CMakeCache.txt')) -and -not (Test-Path (PathOf $build 'build.ninja'))) {
    Info 'the build folder was configured with another generator; starting it again'
    Remove-Item -Recurse -Force $build
}
$preset = if ($OnWindows) { 'clang' } else { 'release' }
$buildType = ''
if (Test-Path (PathOf $build 'CMakeCache.txt')) {
    $line = Select-String -Path (PathOf $build 'CMakeCache.txt') -Pattern '^CMAKE_BUILD_TYPE:\w+=(.*)$' | Select-Object -First 1
    if ($line) { $buildType = $line.Matches[0].Groups[1].Value.Trim() }
}
if (-not (Test-Path (PathOf $build 'build.ninja')) -or $buildType -ne 'Release') {   # a Debug build left by an IDE is slow and needs far more memory to compile
    Info "configuring: cmake --preset $preset (the first run fetches the model store and GoogleTest from GitHub) ..."
    Push-Location $src
    try { Invoke-Native { cmake --preset $preset } } finally { Pop-Location }
    if ($LASTEXITCODE) { Fail 'cmake could not configure the project (see above). The usual causes are no network access to github.com or a toolchain that is not usable.' }
}
Info 'compiling ...'
Invoke-Native { cmake --build $build }
if ($LASTEXITCODE) { Fail 'the build failed (see above)' }
$server = PathOf $build "dnd_server$ExeSuffix"
$sim = PathOf $build "dnd_sim$ExeSuffix"
foreach ($e in @($sim, $server)) { if (-not (Test-Path $e)) { Fail "$e was not produced" } }
Ok "built dnd_sim$ExeSuffix and dnd_server$ExeSuffix"

# ---- 6. tests
if ($NoTests) { Head 'Tests'; Info 'skipped (-NoTests)' }
else {
    Head 'Tests'
    Invoke-Native { ctest --test-dir $build --output-on-failure }
    if ($LASTEXITCODE) { Warn 'some tests failed (see above); the game is built and will run' } else { Ok 'every test passed' }
}

# ---- 7. Ollama and its models
$llmReady = $false
Head 'Ollama (plain-English prompt and the AI table)'
if ($NoLlm) { Info 'skipped (-NoLlm / -Minimal)' }
else {
    $ollamaUrl = 'http://127.0.0.1:11434'
    function Get-OllamaTags { try { return (Invoke-RestMethod -Uri "$ollamaUrl/api/tags" -TimeoutSec 3) } catch { return $null } }
    $ollamaDir = if ($OnWindows) { PathOf $env:LOCALAPPDATA 'Programs' 'Ollama' } elseif ($OnMac) { '/Applications/Ollama.app/Contents/Resources' } else { '' }
    $ollamaPkg = @{ winget = 'Ollama.Ollama'; brew = 'ollama' }
    Add-PathIfThere $ollamaDir
    if (-not (Have 'ollama')) {
        if ($ollamaPkg[$pmKey]) {
            if (Ask "Ollama is not installed. Install it ($(Install-Hint $ollamaPkg))?") { Install-Package $ollamaPkg; Add-PathIfThere $ollamaDir }
        } elseif ($OnLinux -and (Have 'curl')) {
            # no distribution package; ollama.com's script is the supported way (it also sets up a systemd service)
            if (Ask 'Ollama is not installed. Install it with the script from ollama.com (curl -fsSL https://ollama.com/install.sh | sh, uses sudo)?') {
                Invoke-Native { & sh -c 'curl -fsSL https://ollama.com/install.sh | sh' }
                Refresh-Path
            }
        }
    }
    if (-not (Have 'ollama')) { Warn 'Ollama is not installed, so the plain-English prompt and the AI table are off (rerun this later, or install it from https://ollama.com)' }
    else {
        Ok "ollama  $((Get-Command ollama).Source)"
        $tags = Get-OllamaTags
        $started = $null
        if (-not $tags) {
            Info 'starting "ollama serve" for the model downloads ...'
            $sp = @{ FilePath = (Get-Command 'ollama').Source; ArgumentList = 'serve'; PassThru = $true }
            if ($OnWindows) { $sp.WindowStyle = 'Hidden' }
            else { $sp.RedirectStandardOutput = PathOf ([IO.Path]::GetTempPath()) 'cppdnd_ollama.log'; $sp.RedirectStandardError = PathOf ([IO.Path]::GetTempPath()) 'cppdnd_ollama.err.log' }
            $started = Start-Process @sp
            for ($i = 0; $i -lt 30 -and -not $tags; $i++) { Start-Sleep -Seconds 1; $tags = Get-OllamaTags }
        }
        if (-not $tags) { Warn 'Ollama did not answer; models were not checked (start Ollama and run this again)' }
        else {
            $wanted = @()
            try {
                $agents = Read-JsonFile (PathOf $src 'data' 'agents.json')
                $walk = $null
                $walk = { param($o) if ($null -eq $o) { return }
                          if ($o -is [string]) { return }
                          if ($o -is [System.Collections.IEnumerable]) { foreach ($x in $o) { & $walk $x }; return }
                          foreach ($p in $o.PSObject.Properties) { if ($p.Name -eq 'model' -and $p.Value -is [string] -and $p.Value) { $script:wanted += $p.Value } else { & $walk $p.Value } } }
                & $walk $agents
            } catch { Warn "could not read data/agents.json: $($_.Exception.Message)" }
            $wanted = @($wanted | Select-Object -Unique)
            $have = @($tags.models | ForEach-Object { $_.name })
            $missing = @($wanted | Where-Object { $m = $_; -not ($have | Where-Object { $_ -eq $m -or $_ -eq "${m}:latest" }) })
            if ($wanted.Count -eq 0) { Warn 'no models are named in data/agents.json' }
            elseif ($missing.Count -eq 0) { Ok "every model in agents.json is installed ($($wanted -join ', '))" ; $llmReady = $true }
            elseif (Ask "Ollama is missing $($missing.Count) model(s): $($missing -join ', '). Pull them now (several GB)?") {
                foreach ($m in $missing) {
                    Info "ollama pull $m"
                    Invoke-Native { ollama pull $m }
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
    $sd = PathOf $root 'sd.ps1'
    $gpu = $null
    if (Have 'nvidia-smi') { $gpu = Invoke-Native { & nvidia-smi --query-gpu=name --format=csv,noheader 2>$null | Select-Object -First 1 } }
    if (-not $gpu -and $OnWindows) { try { $gpu = (Get-CimInstance Win32_VideoController | Where-Object { $_.Name -match 'NVIDIA' } | Select-Object -First 1).Name } catch {} }
    if (-not $gpu -and $OnMac -and ("$(& uname -m 2>$null)".Trim() -eq 'arm64')) { $gpu = 'Apple Silicon (Metal)' }
    if (-not (Test-Path $sd)) { Warn 'sd.ps1 is missing; pictures are not set up' }
    elseif (-not $gpu) { Warn 'no NVIDIA GPU found, so picture generation is skipped (./sd.ps1 -Install -Cpu works without one but takes minutes per picture)' }
    else {
        Ok "GPU: $gpu"
        if (Ask 'Install Stable Diffusion (AUTOMATIC1111, Python 3.10, the SD 1.5 model: about 15 GB)?') {
            $a = @('-Install'); if ($Yes) { $a += '-Yes' }
            & $sd @a
            if ($LASTEXITCODE) { Warn 'sd.ps1 -Install did not finish; run it again later (it resumes)' } else { $artReady = $true; Ok 'Stable Diffusion installed (the first start also installs PyTorch, 10-20 minutes)' }
        } else { Info 'skipped; run ./sd.ps1 -Install later' }
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
Write-Host '  ./web.ps1                      web client at http://127.0.0.1:8088/ (starts Ollama and Stable Diffusion if they are set up)'
Write-Host '  ./run.ps1 --module meridian    terminal client'
Write-Host '  ./check.ps1                    report what is missing'
