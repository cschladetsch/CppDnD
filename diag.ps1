#!/usr/bin/env pwsh
# Collects what is needed to work out why Stable Diffusion is not answering, into diag.txt (next to this script).
# Read-only: it starts nothing and changes nothing. Paste the contents of diag.txt back, or run with -Clip to copy it.
# PowerShell 7 on Windows, Linux or macOS (or Windows PowerShell 5.1).
#   ./diag.ps1
#   ./diag.ps1 -Clip
param([int]$Port = 7860, [int]$Tail = 60, [switch]$Clip)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path (Join-Path $root 'scripts') 'common.ps1')
$out  = PathOf $root 'diag.txt'
$sdRoot = if ($env:DND_SD_HOME) { $env:DND_SD_HOME } elseif ($OnWindows) { PathOf (Get-DataHome) 'CppDnD' 'sd' } else { PathOf (Get-DataHome) 'cppdnd' 'sd' }
$webui  = PathOf $sdRoot 'stable-diffusion-webui'
$launcherName = if ($OnWindows) { 'webui.bat' } else { 'webui.sh' }
$venvPy = if ($OnWindows) { PathOf $webui 'venv' 'Scripts' 'python.exe' } else { PathOf $webui 'venv' 'bin' 'python' }
$lines  = New-Object System.Collections.Generic.List[string]

function Sect($t) { $lines.Add(''); $lines.Add("=== $t ===") }
function Add($t)  { foreach ($l in @("$t" -split "`r?`n")) { $lines.Add($l) } }
function Try-Run($t, [scriptblock]$b) {
    Sect $t
    try { $r = & $b *>&1 | Out-String; if ($r.Trim()) { Add $r.TrimEnd() } else { Add '(no output)' } }
    catch { Add "failed: $($_.Exception.Message)" }
}
function Tail-File($path) {
    Sect "tail of $path"
    if (Test-Path $path) {
        $f = Get-Item $path
        Add "size $($f.Length) bytes, modified $($f.LastWriteTime)"
        Add ((Get-Content $path -Tail $Tail -ErrorAction SilentlyContinue) -join "`n")
    } else { Add '(missing)' }
}

Sect 'when and where'
Add "$(Get-Date -Format o)  PowerShell $($PSVersionTable.PSVersion)  $([System.Environment]::OSVersion.VersionString)"
Add "repo: $root"
Add "sd home: $sdRoot  (DND_SD_HOME=$($env:DND_SD_HOME))"
Add "DND_SD_ARGS=$($env:DND_SD_ARGS)  DND_SD_REPO=$($env:DND_SD_REPO)  DEEPSEEK_MODEL_HOME=$($env:DEEPSEEK_MODEL_HOME)"
Add "COMMANDLINE_ARGS=$($env:COMMANDLINE_ARGS)  PYTHON=$($env:PYTHON)"

Try-Run 'sd.ps1 -Check' { & (PathOf $root 'sd.ps1') -Check }

Sect 'port'
if ($OnWindows) {
    $conns = Get-NetTCPConnection -LocalPort $Port -ErrorAction SilentlyContinue
    if ($conns) { foreach ($c in $conns) { $p = Get-Process -Id $c.OwningProcess -ErrorAction SilentlyContinue; Add "port $Port $($c.State) pid $($c.OwningProcess) $($p.ProcessName)" } }
    else { Add "nothing is bound to port $Port" }
} else {
    $r = if (Have 'ss') { & ss -ltnp "sport = :$Port" 2>&1 | Out-String } elseif (Have 'lsof') { & lsof -nP -iTCP:$Port -sTCP:LISTEN 2>&1 | Out-String } else { '' }
    if ("$r".Trim()) { Add "$r".TrimEnd() } else { Add "nothing is bound to port $Port (or neither ss nor lsof is installed)" }
}

Sect 'does the API answer'
foreach ($u in @("http://127.0.0.1:$Port/sdapi/v1/sd-models", "http://127.0.0.1:$Port/")) {
    try { $r = Invoke-WebRequest -Uri $u -UseBasicParsing -TimeoutSec 8; Add "$u -> HTTP $($r.StatusCode), $($r.Content.Length) bytes" }
    catch { Add "$u -> $($_.Exception.Message)" }
}

Sect 'processes that may be the image server'
if ($OnWindows) {
    $ps = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'python|webui|cmd' -and ($_.CommandLine -match 'webui|launch\.py|stable-diffusion|7860') }
    if ($ps) { foreach ($p in $ps) { Add "pid $($p.ProcessId) started $($p.CreationDate) $($p.Name): $($p.CommandLine)" } } else { Add '(none running)' }
} else {
    $ps = @(& ps -eo pid,lstart,args 2>$null | Where-Object { $_ -match 'webui|launch\.py|stable-diffusion|7860' -and $_ -notmatch 'diag\.ps1' })
    if ($ps) { foreach ($l in $ps) { Add "$l".Trim() } } else { Add '(none running)' }
}
$pidFile = Get-ChildItem $sdRoot -Filter '*.pid' -ErrorAction SilentlyContinue
foreach ($f in $pidFile) { $id = (Get-Content $f.FullName -ErrorAction SilentlyContinue | Select-Object -First 1); Add "$($f.Name): pid $id, alive: $([bool](Get-Process -Id ([int]$id) -ErrorAction SilentlyContinue))" }

Sect 'install on disk'
Add "webui folder exists: $(Test-Path $webui)"
Add "launcher ($launcherName): $(Test-Path (PathOf $webui $launcherName))  venv python: $(Test-Path $venvPy)"
if (Test-Path $venvPy) {
    Try-Run 'venv python and torch' { & $venvPy -c "import sys;print(sys.version);import torch;print('torch',torch.__version__,'cuda',torch.cuda.is_available(),torch.version.cuda)" }
}
if (Test-Path $sdRoot) { Try-Run 'files in the sd home' { Get-ChildItem $sdRoot | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize } }
if (Test-Path (PathOf $webui '.git')) { Try-Run 'webui git version' { git -C $webui log -1 --format='%h %ad %s' --date=short } }

foreach ($f in (Get-ChildItem $sdRoot -Filter '*.log' -ErrorAction SilentlyContinue)) { Tail-File $f.FullName }
Tail-File (PathOf $webui $(if ($OnWindows) { 'webui-user.bat' } else { 'webui-user.sh' }))

if ($OnWindows) { Try-Run 'python launchers' { py -0p; python --version; where.exe python } }
else { Try-Run 'pythons' { foreach ($n in 'python3.10', 'python3', 'python') { $c = Get-Command $n -ErrorAction SilentlyContinue; if ($c) { "$n -> $($c.Source): $(& $c.Source --version 2>&1)" } } } }
Try-Run 'git' { git --version }
Try-Run 'GPU' { nvidia-smi --query-gpu=name,driver_version,memory.total,memory.used --format=csv }
Try-Run 'disk' { Get-PSDrive -PSProvider FileSystem | Select-Object Name, @{n='FreeGB';e={[math]::Round($_.Free/1GB,1)}}, @{n='UsedGB';e={[math]::Round($_.Used/1GB,1)}} | Format-Table -AutoSize }
if ($OnWindows) { Try-Run 'firewall or proxy hints' { Get-NetFirewallProfile | Select-Object Name, Enabled | Format-Table -AutoSize; "HTTP_PROXY=$($env:HTTP_PROXY) HTTPS_PROXY=$($env:HTTPS_PROXY)" } }
else { Try-Run 'proxy hints' { "HTTP_PROXY=$($env:HTTP_PROXY) HTTPS_PROXY=$($env:HTTPS_PROXY) http_proxy=$($env:http_proxy) https_proxy=$($env:https_proxy)" } }

Sect 'model store'
$store = Get-ModelHome
Add "store: $store"
Try-Run 'sd15 folder' { Get-ChildItem (PathOf $store 'sd15') -ErrorAction Stop | Select-Object Name, @{n='MB';e={[math]::Round($_.Length/1MB)}} | Format-Table -AutoSize }

Sect 'game server config'
Add (Get-Content (PathOf $root 'sim' 'data' 'images.json') -ErrorAction SilentlyContinue | Out-String)

Set-Content -Path $out -Value $lines -Encoding UTF8
Write-Host "Wrote $out ($($lines.Count) lines)." -ForegroundColor Green
if ($Clip) {
    try { Get-Content $out -Raw | Set-Clipboard -ErrorAction Stop; Write-Host 'Copied to the clipboard.' -ForegroundColor Green }   # Linux needs xclip or wl-clipboard for this
    catch { Write-Host "Could not copy to the clipboard ($($_.Exception.Message)); paste $out instead." -ForegroundColor Yellow }
}
