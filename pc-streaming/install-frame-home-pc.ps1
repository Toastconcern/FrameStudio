# Frame Home on a Windows PC running SteamVR: your environments become SteamVR's environment itself — drawn where
# Aurora would be, so they show at boot, during game loads and behind the Steam menu, with the controllers and
# passthrough. The Environments panel in the Steam menu switches them. It shows the environments in Frame Studio's
# My Environments (Documents\Frame Studio\Environments): save one in Studio and it's there.
#
#   powershell -ExecutionPolicy Bypass -File install-frame-home-pc.ps1              install or update
#   powershell -ExecutionPolicy Bypass -File install-frame-home-pc.ps1 -Uninstall   SteamVR's own Aurora back
#
# How: a small Direct3D 11 layer (d3d11.dll beside SteamVR's compositor) draws the home in Aurora's place, fed by
# frame-home --system; the real d3d11.dll is kept beside it as fh_d3d11real.dll and every call it makes passes
# straight through. The Environments panel runs frame-home --system and switches homes. -Uninstall takes the layer
# off (deleting those two files) and SteamVR is exactly as before.
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = $(if ($env:FRAME_HOME_ROOT) { $env:FRAME_HOME_ROOT } else { Join-Path $env:LOCALAPPDATA 'Frame Home' })   # (FRAME_HOME_ROOT: tests)
$bin = Join-Path $root 'bin'
$layer = Join-Path $root 'layer'
function Say($m) { Write-Host "frame-install: $m" }

# SteamVR, from OpenVR's own record of where things are: its config folder and where it's installed (for the compositor).
$vrpath = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
if (-not (Test-Path $vrpath)) { Say "SteamVR isn't installed on this PC (no $vrpath). Install SteamVR from Steam first."; exit 1 }
$openvr = Get-Content $vrpath -Raw | ConvertFrom-Json
$cfg = @($openvr.config)[0]
$runtime = @($openvr.runtime)[0]
if (-not $cfg -or -not $runtime) { Say "SteamVR's folders aren't listed in $vrpath"; exit 1 }
$bin64 = Join-Path $runtime 'bin\win64'
$proxy = Join-Path $bin64 'd3d11.dll'
$real = Join-Path $bin64 'fh_d3d11real.dll'
$running = [bool](Get-Process vrserver -ErrorAction SilentlyContinue)

function StopHome { Get-Process frame-home, frame-switcher -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 400 }

# JSON as SteamVR writes it: UTF-8 without a byte-order mark.
function Load($p) { if (-not (Test-Path $p)) { return [pscustomobject]@{} } try { return Get-Content $p -Raw | ConvertFrom-Json } catch { Say "$p does not read: left as it is"; exit 1 } }
function Save($p, $o) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $p) | Out-Null
    [IO.File]::WriteAllText("$p.tmp", ($o | ConvertTo-Json -Depth 32), (New-Object Text.UTF8Encoding $false))
    Move-Item -Force "$p.tmp" $p
}
function SetProp($o, $name, $value) { if ($o.PSObject.Properties[$name]) { $o.$name = $value } else { $o | Add-Member -NotePropertyName $name -NotePropertyValue $value } }

# enableHomeApp off (no separate home app) and environmentMode 1 (the compositor draws Aurora, which the layer
# replaces). Saved once as they were, put back on -Uninstall. Also the panel keeps them so while SteamVR runs.
$before = Join-Path $root 'steamvr-before.json'
function Settings($install) {
    $vs = Join-Path $cfg 'steamvr.vrsettings'
    $s = Load $vs
    if (-not $s.PSObject.Properties['steamvr']) { SetProp $s 'steamvr' ([pscustomobject]@{}) }
    $sv = $s.steamvr
    if ($install) {
        if (-not (Test-Path $before)) {
            Save $before ([pscustomobject]@{
                enableHomeApp = [bool]$sv.enableHomeApp
                environmentMode = $(if ($sv.PSObject.Properties['environmentMode']) { [int]$sv.environmentMode } else { 1 })
            })
        }
        SetProp $sv 'enableHomeApp' $false
        SetProp $sv 'environmentMode' 1
    } else {
        $was = $(if (Test-Path $before) { Get-Content $before -Raw | ConvertFrom-Json } else { $null })
        SetProp $sv 'enableHomeApp' $(if ($was) { [bool]$was.enableHomeApp } else { $false })
        SetProp $sv 'environmentMode' $(if ($was) { [int]$was.environmentMode } else { 1 })
        Remove-Item -Force -ErrorAction SilentlyContinue $before
    }
    Save $vs $s
}

# The panel, registered with SteamVR (frame-switcher --install) or, when SteamVR isn't running, in its config files.
$switcherManifest = Join-Path $root 'frame-switcher.vrmanifest'
function PanelOffline($install) {
    $ac = Join-Path $cfg 'appconfig.json'
    $a = Load $ac
    if (-not $a.PSObject.Properties['manifest_paths']) { SetProp $a 'manifest_paths' @() }
    $paths = [System.Collections.ArrayList]@($a.manifest_paths)
    if ($install -and -not $paths.Contains($switcherManifest)) { [void]$paths.Add($switcherManifest) }
    if (-not $install -and $paths.Contains($switcherManifest)) { $paths.Remove($switcherManifest) }
    $a.manifest_paths = @($paths)
    Save $ac $a
    Save (Join-Path $cfg 'vrappconfig\framehome.switcher.vrappconfig') ([pscustomobject]@{ autolaunch = [bool]$install; last_launch_time = '0' })
}

if ($Uninstall) {
    StopHome
    if ($running) { & (Join-Path $bin 'frame-switcher.exe') --uninstall } else { PanelOffline $false }
    Settings $false
    Remove-Item -Force -ErrorAction SilentlyContinue $proxy    # the layer off: SteamVR loads the system d3d11 again
    Remove-Item -Force -ErrorAction SilentlyContinue $real
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $layer
    Say "Frame Home is off: SteamVR's own environment is back (restart SteamVR to see it). Your environments are kept in Documents\Frame Studio\Environments"
    exit 0
}

foreach ($x in 'frame-home.exe', 'frame-switcher.exe', 'd3d11.dll') {
    if (-not (Test-Path (Join-Path $here $x))) { Say "this installer has no $x"; exit 1 }
}
# Frame Home runs on this PC (a missing Vulkan driver shows up now, not as a black world later).
& (Join-Path $here 'frame-home.exe') --version | Out-Null
if ($LASTEXITCODE -ne 0) { Say "Frame Home can't run on this PC (Vulkan driver?): nothing was changed"; exit 1 }

StopHome
New-Item -ItemType Directory -Force -Path $bin, $layer, (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Frame Studio\Environments') | Out-Null
foreach ($x in 'frame-home.exe', 'frame-switcher.exe') { Copy-Item -Force (Join-Path $here $x) (Join-Path $bin $x) }

# The compositor layer: keep the real d3d11.dll (from System32 — SteamVR's bin\win64 has none of its own) as
# fh_d3d11real.dll, then drop our proxy in as d3d11.dll. (We copy the real one only once, never our own proxy.)
if (-not (Test-Path $bin64)) { Say "SteamVR's compositor folder isn't where $vrpath says ($bin64)"; exit 1 }
if (-not (Test-Path $real)) { Copy-Item -Force (Join-Path $env:windir 'System32\d3d11.dll') $real }
Copy-Item -Force (Join-Path $here 'd3d11.dll') $proxy

Settings $true
if ($running) {
    & (Join-Path $bin 'frame-switcher.exe') --install    # registers the panel, auto-launches it, starts it now
    Say "installed. SteamVR is running, so restart it (or your headset) once to load the environment layer; the Environments panel is in the Steam menu."
} else {
    & (Join-Path $bin 'frame-switcher.exe') --install | Out-Null
    PanelOffline $true
    Say "installed: your environment becomes SteamVR's when SteamVR next starts."
}
exit 0
