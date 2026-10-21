# Frame Home on a Windows PC running SteamVR: your homes become SteamVR's environment, and the Environments panel
# in the Steam menu switches them. It shows the environments in Frame Studio's My Environments
# (Documents\Frame Studio\Environments): save one in Studio and it's there.
#
#   powershell -ExecutionPolicy Bypass -File install-frame-home-pc.ps1              install or update
#   powershell -ExecutionPolicy Bypass -File install-frame-home-pc.ps1 -Uninstall   SteamVR's own environment back
#
# How: Frame Home becomes SteamVR's home app (SteamVR runs it whenever no game is open), with SteamVR's classic
# environment in place of Aurora showing a 360-degree picture of the current home for the moments before Frame Home
# draws. The SteamVR settings this changes are saved first and put back exactly on -Uninstall. Works with SteamVR
# running (through its API) or not (its config files, read when it next starts).
param([switch]$Uninstall)
$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = $(if ($env:FRAME_HOME_ROOT) { $env:FRAME_HOME_ROOT } else { Join-Path $env:LOCALAPPDATA 'Frame Home' })   # (FRAME_HOME_ROOT: tests)
$bin = Join-Path $root 'bin'
function Say($m) { Write-Host "frame-install: $m" }

# SteamVR's config folder, from OpenVR's own record of where things are.
$vrpath = Join-Path $env:LOCALAPPDATA 'openvr\openvrpaths.vrpath'
if (-not (Test-Path $vrpath)) { Say "SteamVR isn't installed on this PC (no $vrpath). Install SteamVR from Steam first."; exit 1 }
$openvr = Get-Content $vrpath -Raw | ConvertFrom-Json
$cfg = @($openvr.config)[0]
if (-not $cfg) { Say "SteamVR's config folder isn't listed in $vrpath"; exit 1 }
$running = [bool](Get-Process vrserver -ErrorAction SilentlyContinue)

# JSON as SteamVR writes it: UTF-8 without a byte-order mark, replaced in one step.
function Load($p) {
    if (-not (Test-Path $p)) { return [pscustomobject]@{} }
    try { return Get-Content $p -Raw | ConvertFrom-Json } catch { Say "$p does not read: left as it is"; exit 1 }
}
function Save($p, $o) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $p) | Out-Null
    $json = $o | ConvertTo-Json -Depth 32
    [IO.File]::WriteAllText("$p.tmp", $json, (New-Object Text.UTF8Encoding $false))
    Move-Item -Force "$p.tmp" $p
}
function SetProp($o, $name, $value) {
    if ($o.PSObject.Properties[$name]) { $o.$name = $value } else { $o | Add-Member -NotePropertyName $name -NotePropertyValue $value }
}

$manifests = @((Join-Path $root 'frame-home.vrmanifest'), (Join-Path $root 'frame-switcher.vrmanifest'))
$before = Join-Path $root 'steamvr-before.json'

# SteamVR not running: the same registration, in its config files.
function Offline($install) {
    $ac = Join-Path $cfg 'appconfig.json'
    $a = Load $ac
    if (-not $a.PSObject.Properties['manifest_paths']) { SetProp $a 'manifest_paths' @() }
    $paths = [System.Collections.ArrayList]@($a.manifest_paths)
    foreach ($m in $manifests) {
        if ($install -and -not $paths.Contains($m)) { [void]$paths.Add($m) }
        if (-not $install -and $paths.Contains($m)) { $paths.Remove($m) }
    }
    $a.manifest_paths = @($paths)
    Save $ac $a
    $vs = Join-Path $cfg 'steamvr.vrsettings'
    $s = Load $vs
    if (-not $s.PSObject.Properties['steamvr']) { SetProp $s 'steamvr' ([pscustomobject]@{}) }
    $sv = $s.steamvr
    if ($install) {
        if (-not (Test-Path $before)) {   # what was there before Frame Home, saved once
            $was = [ordered]@{
                enableHomeApp = [bool]$sv.enableHomeApp
                environmentMode = $(if ($sv.PSObject.Properties['environmentMode']) { [int]$sv.environmentMode } else { 1 })
                background = [string]$sv.background
                backgroundUseDomeProjection = [bool]$sv.backgroundUseDomeProjection
                backgroundCameraHeight = $(if ($sv.PSObject.Properties['backgroundCameraHeight']) { [double]$sv.backgroundCameraHeight } else { 1.6 })
                backgroundDomeRadius = [double]$sv.backgroundDomeRadius
            }
            Save $before ([pscustomobject]$was)
        }
        SetProp $sv 'enableHomeApp' $true
        SetProp $sv 'environmentMode' 0
    } else {
        $was = $(if (Test-Path $before) { Get-Content $before -Raw | ConvertFrom-Json } else { $null })
        SetProp $sv 'enableHomeApp' $(if ($was) { [bool]$was.enableHomeApp } else { $false })
        SetProp $sv 'environmentMode' $(if ($was) { [int]$was.environmentMode } else { 1 })
        SetProp $sv 'background' $(if ($was) { [string]$was.background } else { '' })
        SetProp $sv 'backgroundUseDomeProjection' $(if ($was) { [bool]$was.backgroundUseDomeProjection } else { $false })
        if ($was) { SetProp $sv 'backgroundCameraHeight' ([double]$was.backgroundCameraHeight); SetProp $sv 'backgroundDomeRadius' ([double]$was.backgroundDomeRadius) }
        Remove-Item -Force -ErrorAction SilentlyContinue $before
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue (Join-Path $root 'background')
        foreach ($m in $manifests) { Remove-Item -Force -ErrorAction SilentlyContinue $m }
    }
    Save $vs $s
    Save (Join-Path $cfg 'vrappconfig\framehome.switcher.vrappconfig') ([pscustomobject]@{ autolaunch = [bool]$install; last_launch_time = '0' })
}

function StopAll { Get-Process frame-home, frame-switcher -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep -Milliseconds 500 }

if ($Uninstall) {
    if ($running) {
        & (Join-Path $bin 'frame-home.exe') --uninstall
        & (Join-Path $bin 'frame-switcher.exe') --uninstall
    } else {
        Offline $false
    }
    StopAll
    Say "SteamVR's own environment is back (your environments are kept in Documents\Frame Studio\Environments)"
    exit 0
}

foreach ($x in 'frame-home.exe', 'frame-switcher.exe') {
    if (-not (Test-Path (Join-Path $here $x))) { Say "this installer has no $x"; exit 1 }
}
# Runs here, before anything installed is touched (a missing Vulkan driver shows up now, not as a black world later).
& (Join-Path $here 'frame-home.exe') --version | Out-Null
if ($LASTEXITCODE -ne 0) { Say "Frame Home can't run on this PC (Vulkan driver?): nothing was changed"; exit 1 }

StopAll
New-Item -ItemType Directory -Force -Path $bin, (Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Frame Studio\Environments') | Out-Null
foreach ($x in 'frame-home.exe', 'frame-switcher.exe') { Copy-Item -Force (Join-Path $here $x) (Join-Path $bin $x) }

if ($running) {
    & (Join-Path $here 'frame-switcher.exe') --install
    & (Join-Path $here 'frame-home.exe') --install
    if ($LASTEXITCODE -ne 0) { Say "SteamVR refused Frame Home ($LASTEXITCODE)"; exit 1 }
    Say "installed: Frame Home is SteamVR's environment now"
} else {
    # (with SteamVR not running these write their manifests and say so)
    & (Join-Path $here 'frame-switcher.exe') --install | Out-Null
    & (Join-Path $here 'frame-home.exe') --install | Out-Null
    Offline $true
    Say "installed: Frame Home becomes SteamVR's environment when SteamVR next starts"
}
exit 0
