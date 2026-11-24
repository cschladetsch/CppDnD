# Shared by the CppDnD scripts (dot-source it): which platform this is, where things live, installing a package with the
# platform's package manager, configuring and building with the CMake presets, starting a background process and stopping
# a process tree. Works in PowerShell 7 on Windows, Linux and macOS, and in Windows PowerShell 5.1.
# install.ps1 does not use this file, because it has to work as a single downloaded file; it carries its own copy of these.

# $IsWindows and friends do not exist in Windows PowerShell 5.1, so Windows is detected from the environment.
$script:OnWindows = ($env:OS -eq 'Windows_NT')
$script:OnMac     = (-not $script:OnWindows) -and ($IsMacOS -eq $true)
$script:OnLinux   = (-not $script:OnWindows) -and (-not $script:OnMac)
$script:ExeSuffix = if ($script:OnWindows) { '.exe' } else { '' }

function Exe($name) { return "$name$script:ExeSuffix" }

# Join any number of path parts with this platform's separator: PathOf $HOME '.local' 'share'
function PathOf { return [IO.Path]::Combine([string[]]$args) }

function Have($name) { return [bool](Get-Command $name -ErrorAction SilentlyContinue) }

# Per-user application data: %LOCALAPPDATA% on Windows, $XDG_DATA_HOME or ~/.local/share elsewhere (macOS too, which keeps it simple).
function Get-DataHome {
    if ($script:OnWindows) { return $env:LOCALAPPDATA }
    if ($env:XDG_DATA_HOME) { return $env:XDG_DATA_HOME }
    return (PathOf $HOME '.local' 'share')
}
# The CppLmmModelStore; the C++ code uses the same default.
function Get-ModelHome {
    if ($env:DEEPSEEK_MODEL_HOME) { return $env:DEEPSEEK_MODEL_HOME }
    return (PathOf $HOME '.local' 'share' 'deepseek' 'models')
}

function Add-PathIfThere($dir) {
    $sep = [IO.Path]::PathSeparator
    if ($dir -and (Test-Path $dir) -and (($env:PATH -split [regex]::Escape($sep)) -notcontains $dir)) { $env:PATH = "$env:PATH$sep$dir" }
}
# Pick up tools a package manager has just installed, without opening a new terminal.
function Refresh-Path {
    if ($script:OnWindows) {
        $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
    } else {
        foreach ($d in '/opt/homebrew/bin', '/usr/local/bin', '/home/linuxbrew/.linuxbrew/bin', '/snap/bin', (PathOf $HOME '.local' 'bin')) { Add-PathIfThere $d }
    }
}

function Read-JsonFile($file) {
    return ((Get-Content $file -Raw) -replace '(?s)/\*.*?\*/', '' -replace '(?m)^\s*//.*$', '') | ConvertFrom-Json
}

# ---- packages
# A package is a hashtable naming it for each manager, e.g. @{ winget = 'Kitware.CMake'; apt = 'cmake'; dnf = 'cmake'; pacman = 'cmake'; brew = 'cmake' }.
# A value may be an array (several packages, or brew's '--cask', 'blender'). wingetArgs adds arguments to winget.
function Get-PackageManager {
    if ($script:OnWindows) { if (Have 'winget') { return 'winget' } else { return $null } }
    if ($script:OnMac) { if (Have 'brew') { return 'brew' } else { return $null } }
    foreach ($m in 'apt-get', 'dnf', 'pacman', 'zypper', 'brew') { if (Have $m) { return $m } }
    return $null
}
function Get-PackageName($pkg) {
    $pm = Get-PackageManager
    if (-not $pm) { return $null }
    $key = if ($pm -eq 'apt-get') { 'apt' } else { $pm }
    return $pkg[$key]
}
# The command a person would type, for messages ("winget install -e --id Kitware.CMake", "sudo apt-get install cmake").
function Get-InstallHint($pkg) {
    $pm = Get-PackageManager
    $name = Get-PackageName $pkg
    if (-not $pm) {
        if ($script:OnWindows) { return 'install winget ("App Installer" from the Microsoft Store)' }
        if ($script:OnMac) { return 'install Homebrew (https://brew.sh)' }
        return 'install it with your package manager'
    }
    if (-not $name) { return "it has no $pm package; see its website" }
    $n = @($name) -join ' '
    switch ($pm) {
        'winget'  { return "winget install -e --id $n" }
        'apt-get' { return "sudo apt-get install $n" }
        'dnf'     { return "sudo dnf install $n" }
        'pacman'  { return "sudo pacman -S $n" }
        'zypper'  { return "sudo zypper install $n" }
        'brew'    { return "brew install $n" }
    }
}
function Test-Root {
    if ($script:OnWindows) { return $false }
    try { return ((& id -u) -eq '0') } catch { return $false }
}
$script:aptUpdated = $false
# Returns $true when the package manager ran (the caller then checks for the tool, since "already installed" is not an error).
function Install-Package($pkg) {
    $pm = Get-PackageManager
    $name = Get-PackageName $pkg
    if (-not $pm -or -not $name) { return $false }
    $names = @($name)
    $sudo = if ($pm -ne 'brew' -and $pm -ne 'winget' -and -not (Test-Root)) { 'sudo' } else { $null }
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # package managers write progress to stderr
    try {
        switch ($pm) {
            'winget' {
                $a = @('install', '-e', '--id') + $names + @('--silent', '--accept-package-agreements', '--accept-source-agreements')
                if ($pkg.wingetArgs) { $a += $pkg.wingetArgs }
                & winget @a
            }
            'apt-get' {
                if (-not $script:aptUpdated) { if ($sudo) { & sudo apt-get update } else { & apt-get update }; $script:aptUpdated = $true }
                if ($sudo) { & sudo apt-get install -y @names } else { & apt-get install -y @names }
            }
            'dnf'    { if ($sudo) { & sudo dnf install -y @names } else { & dnf install -y @names } }
            'pacman' { if ($sudo) { & sudo pacman -S --needed --noconfirm @names } else { & pacman -S --needed --noconfirm @names } }
            'zypper' { if ($sudo) { & sudo zypper --non-interactive install @names } else { & zypper --non-interactive install @names } }
            'brew'   { & brew install @names }
        }
    } finally { $ErrorActionPreference = $eap }
    Refresh-Path
    return $true
}

# ---- build
# CMakePresets.json in sim: "clang" on Windows (clang++ with the Visual Studio library and SDK, no developer prompt needed),
# "release" elsewhere (the default C++ compiler, or CXX). Both build Release with Ninja into sim/build.
function Get-BuildPreset { if ($script:OnWindows) { return 'clang' } else { return 'release' } }
function Get-CMakeVersion {
    if (-not (Have 'cmake')) { return $null }
    $line = (& cmake --version 2>$null | Select-Object -First 1)
    if ("$line" -match '(\d+)\.(\d+)') { return [version]"$($Matches[1]).$($Matches[2])" }
    return $null
}
# Configure sim/build unless it is already configured for Ninja. Returns the exit code (0 when fine).
function Invoke-Configure($src, [switch]$Quiet) {
    $build = PathOf $src 'build'
    if ((Test-Path (PathOf $build 'CMakeCache.txt')) -and -not (Test-Path (PathOf $build 'build.ninja'))) {
        Write-Host '         sim/build was configured with another generator; starting it again' -ForegroundColor DarkGray
        Remove-Item -Recurse -Force $build
    }
    # A build folder configured by hand or by an IDE may be a Debug build (-O0 -g): far slower to play, and clang++ needs much
    # more memory for it, enough to fail with "out of memory" while llama-server and Stable Diffusion are loaded. Make it Release.
    $type = ''
    $cache = PathOf $build 'CMakeCache.txt'
    if (Test-Path $cache) {
        $line = Select-String -Path $cache -Pattern '^CMAKE_BUILD_TYPE:\w+=(.*)$' | Select-Object -First 1
        if ($line) { $type = $line.Matches[0].Groups[1].Value.Trim() }
    }
    if ((Test-Path (PathOf $build 'build.ninja')) -and $type -eq 'Release') { return 0 }
    if (Test-Path (PathOf $build 'build.ninja')) {
        Write-Host "         sim/build is a $(if ($type) { $type } else { 'no-type' }) build; configuring it as Release (cmake --preset $(Get-BuildPreset))" -ForegroundColor DarkGray
    }
    $v = Get-CMakeVersion
    if ($v -and $v -lt [version]'3.21') { Write-Host "  [FAIL] cmake $v is too old for CMakePresets.json; 3.21 or newer is needed" -ForegroundColor Red; return 1 }
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    Push-Location $src
    try {
        if ($Quiet) { & cmake --preset (Get-BuildPreset) | Out-Null } else { & cmake --preset (Get-BuildPreset) | Out-Host }
        return $LASTEXITCODE
    } finally { Pop-Location; $ErrorActionPreference = $eap }
}

# ---- processes
# Quote one argument for Start-Process, which joins -ArgumentList with spaces on every platform.
function Quote-Arg([string]$a) {
    if ($a -eq '') { return '""' }
    if ($a -notmatch '[\s"]') { return $a }
    return '"' + ($a -replace '(\\*)"', '$1$1\"' -replace '(\\+)$', '$1$1') + '"'
}
# Start a program in the background. On Windows it gets a hidden window; elsewhere its output goes to $Log (and $Log.err),
# so it does not write over this terminal. Returns the process.
function Start-Background {
    param([string]$FilePath, [string[]]$ArgumentList = @(), [string]$WorkingDirectory = '', [string]$Log = '', [string]$ErrLog = '')
    $p = @{ FilePath = $FilePath; PassThru = $true }
    if ($ArgumentList.Count) { $p.ArgumentList = @($ArgumentList | ForEach-Object { Quote-Arg $_ }) }
    if ($WorkingDirectory) { $p.WorkingDirectory = $WorkingDirectory }
    if (-not $Log -and -not $script:OnWindows) { $Log = PathOf ([IO.Path]::GetTempPath()) ("cppdnd_" + [IO.Path]::GetFileNameWithoutExtension($FilePath) + '.log') }
    if ($Log) {
        if (-not $ErrLog) { $ErrLog = "$Log.err" }
        $p.RedirectStandardOutput = $Log
        $p.RedirectStandardError = $ErrLog
    }
    if ($script:OnWindows) { $p.WindowStyle = 'Hidden' }
    return (Start-Process @p)
}
# Stop a process and everything it started (launch.py starts the web UI's Python server; ollama serve starts runners).
function Stop-Tree($id) {
    if (-not $id) { return }
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # taskkill writes to stderr when the process has gone
    try {
        if ($script:OnWindows) { & taskkill.exe /PID $id /T /F 2>&1 | Out-Null; return }
        try { [Diagnostics.Process]::GetProcessById([int]$id).Kill($true) }   # .NET Core 3+: the whole tree
        catch { Stop-Process -Id $id -Force -ErrorAction SilentlyContinue }
    } catch {} finally { $ErrorActionPreference = $eap }
}
# Is a TCP port on 127.0.0.1 free?
function Test-PortFree([int]$port) {
    try {
        $l = New-Object System.Net.Sockets.TcpListener ([System.Net.IPAddress]::Loopback), $port
        $l.Start(); $l.Stop(); return $true
    } catch { return $false }
}
# Open a URL in the default browser.
function Open-Url($u) {
    try {
        if ($script:OnWindows) { Start-Process $u }
        elseif ($script:OnMac) { & open $u }
        elseif (Have 'xdg-open') { & xdg-open $u *> $null }
        else { Write-Host "         open $u in a browser" -ForegroundColor DarkGray }
    } catch { Write-Host "         open $u in a browser" -ForegroundColor DarkGray }
}
# Is there an NVIDIA GPU? Returns @{ Name; MiB; Driver } or $null.
function Get-NvidiaGpu {
    if (-not (Have 'nvidia-smi')) { return $null }
    $eap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { $line = (& nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader,nounits 2>$null | Select-Object -First 1) }
    finally { $ErrorActionPreference = $eap }
    if (-not $line) { return $null }
    $f = "$line".Split(',') | ForEach-Object { $_.Trim() }
    if ($f.Count -lt 3) { return $null }
    return @{ Name = $f[0]; MiB = [int]$f[1]; Driver = $f[2] }
}
