# Collects what is needed to work out why Stable Diffusion is not answering, into diag.txt (next to this script).
# Read-only: it starts nothing and changes nothing. Paste the contents of diag.txt back, or run with -Clip to copy it.
#   .\diag.ps1
#   .\diag.ps1 -Clip
param([int]$Port = 7860, [int]$Tail = 60, [switch]$Clip)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $root 'diag.txt'
$sdRoot = if ($env:DND_SD_HOME) { $env:DND_SD_HOME } else { Join-Path $env:LOCALAPPDATA 'CppDnD\sd' }
$webui  = Join-Path $sdRoot 'stable-diffusion-webui'
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

Try-Run 'sd.ps1 -Check' { & (Join-Path $root 'sd.ps1') -Check }

Sect 'port'
$conns = Get-NetTCPConnection -LocalPort $Port -ErrorAction SilentlyContinue
if ($conns) { foreach ($c in $conns) { $p = Get-Process -Id $c.OwningProcess -ErrorAction SilentlyContinue; Add "port $Port $($c.State) pid $($c.OwningProcess) $($p.ProcessName)" } }
else { Add "nothing is bound to port $Port" }

Sect 'does the API answer'
foreach ($u in @("http://127.0.0.1:$Port/sdapi/v1/sd-models", "http://127.0.0.1:$Port/")) {
    try { $r = Invoke-WebRequest -Uri $u -UseBasicParsing -TimeoutSec 8; Add "$u -> HTTP $($r.StatusCode), $($r.Content.Length) bytes" }
    catch { Add "$u -> $($_.Exception.Message)" }
}

Sect 'processes that may be the image server'
$ps = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue | Where-Object { $_.Name -match 'python|webui|cmd' -and ($_.CommandLine -match 'webui|launch\.py|stable-diffusion|7860') }
if ($ps) { foreach ($p in $ps) { Add "pid $($p.ProcessId) started $($p.CreationDate) $($p.Name): $($p.CommandLine)" } } else { Add '(none running)' }
$pidFile = Get-ChildItem $sdRoot -Filter '*.pid' -ErrorAction SilentlyContinue
foreach ($f in $pidFile) { $id = (Get-Content $f.FullName -ErrorAction SilentlyContinue | Select-Object -First 1); Add "$($f.Name): pid $id, alive: $([bool](Get-Process -Id ([int]$id) -ErrorAction SilentlyContinue))" }

Sect 'install on disk'
Add "webui folder exists: $(Test-Path $webui)"
Add "launcher: $(Test-Path (Join-Path $webui 'webui.bat'))  venv python: $(Test-Path (Join-Path $webui 'venv\Scripts\python.exe'))"
$venvPy = Join-Path $webui 'venv\Scripts\python.exe'
if (Test-Path $venvPy) {
    Try-Run 'venv python and torch' { & $venvPy -c "import sys;print(sys.version);import torch;print('torch',torch.__version__,'cuda',torch.cuda.is_available(),torch.version.cuda)" }
}
if (Test-Path $sdRoot) { Try-Run 'files in the sd home' { Get-ChildItem $sdRoot | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize } }
if (Test-Path (Join-Path $webui '.git')) { Try-Run 'webui git version' { git -C $webui log -1 --format='%h %ad %s' --date=short } }

foreach ($f in (Get-ChildItem $sdRoot -Filter '*.log' -ErrorAction SilentlyContinue)) { Tail-File $f.FullName }
Tail-File (Join-Path $webui 'webui-user.bat')

Try-Run 'python launchers' { py -0p; python --version; where.exe python }
Try-Run 'git' { git --version }
Try-Run 'GPU' { nvidia-smi --query-gpu=name,driver_version,memory.total,memory.used --format=csv }
Try-Run 'disk' { Get-PSDrive -PSProvider FileSystem | Select-Object Name, @{n='FreeGB';e={[math]::Round($_.Free/1GB,1)}}, @{n='UsedGB';e={[math]::Round($_.Used/1GB,1)}} | Format-Table -AutoSize }
Try-Run 'firewall or proxy hints' { Get-NetFirewallProfile | Select-Object Name, Enabled | Format-Table -AutoSize; "HTTP_PROXY=$($env:HTTP_PROXY) HTTPS_PROXY=$($env:HTTPS_PROXY)" }

Sect 'model store'
$store = if ($env:DEEPSEEK_MODEL_HOME) { $env:DEEPSEEK_MODEL_HOME } else { Join-Path $env:USERPROFILE '.local\share\deepseek\models' }
Add "store: $store"
Try-Run 'sd15 folder' { Get-ChildItem (Join-Path $store 'sd15') -ErrorAction Stop | Select-Object Name, @{n='MB';e={[math]::Round($_.Length/1MB)}} | Format-Table -AutoSize }

Sect 'game server config'
Add (Get-Content (Join-Path $root 'sim\data\images.json') -ErrorAction SilentlyContinue | Out-String)

Set-Content -Path $out -Value $lines -Encoding UTF8
Write-Host "Wrote $out ($($lines.Count) lines)." -ForegroundColor Green
if ($Clip) { Get-Content $out -Raw | Set-Clipboard; Write-Host 'Copied to the clipboard.' -ForegroundColor Green }
