# Installs Frame Home on a Steam Frame from a Windows PC, without Frame Studio: the same steps as Studio's
# Frame > Install on Steam Frame. Needs ssh access to the headset (its developer key, or a password) and an ssh
# client: Windows' own OpenSSH (Settings > Optional features) or Git for Windows'.
#
#   powershell -ExecutionPolicy Bypass -File install-frame-home.ps1 [-HostName HOST] [-Uninstall] [environment.fenv | bundle.fbundle ...]
#
# HOST: the headset's ssh address (default "steamos@frame": an entry named frame in ~/.ssh/config, or the headset's
# name on the network; an IP works too, e.g. steamos@192.168.1.50). Environments (.fenv) given are copied into its
# selector as well; the first .fenv is the one shown. Bundles (.fbundle) are copied as they are and the headset's
# Environments panel unpacks them into their environments (they never change the one shown). -Uninstall gives the
# environment back to SteamVR (files stay).
param(
    [string]$HostName = "steamos@frame",
    [switch]$Uninstall,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Environments = @()
)
$ErrorActionPreference = "Stop"
function Say($text) { Write-Host "frame-home: $text" }

# ssh / scp: Windows' OpenSSH, else Git for Windows' copy (as Frame Studio finds them)
function Find-Tool($name) {
    $onPath = Get-Command "$name.exe" -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    $tries = @("$env:WINDIR\System32\OpenSSH\$name.exe")
    foreach ($root in @($env:ProgramW6432, $env:ProgramFiles, $env:LOCALAPPDATA)) {
        if ($root) { $tries += "$root\Git\usr\bin\$name.exe"; $tries += "$root\Programs\Git\usr\bin\$name.exe" }
    }
    foreach ($t in $tries) { if (Test-Path $t) { return $t } }
    throw "No $name found: turn on Windows' OpenSSH Client (Settings > Apps > Optional features), or install Git for Windows."
}
$ssh = Find-Tool "ssh"
$scp = Find-Tool "scp"
if ($HostName -notmatch "@") { $HostName = "steamos@$HostName" }

# The name an environment gets on the headset: lowercase, spaces as dashes, only [a-z0-9._-] (as Studio names them).
function Frame-Name($path) {
    $n = ([IO.Path]::GetFileName($path)).ToLowerInvariant() -replace " ", "-"
    return ($n -replace "[^a-z0-9._-]", "")
}

$payload = Join-Path $PSScriptRoot "payload"
if (-not $Uninstall -and -not (Test-Path (Join-Path $payload "install.sh"))) { throw "The headset files are missing: expected $payload\install.sh" }

Say "connecting to $HostName"
$arch = & $ssh -o ConnectTimeout=8 $HostName "mkdir -p frame-home/envs frame-home/.install && uname -m"
if ($LASTEXITCODE -ne 0) { throw "Can't reach $HostName over ssh (is the headset on, in developer mode, with ssh enabled?)" }
if ("$arch".Trim() -ne "aarch64") { throw "$HostName is a $arch machine, not a Steam Frame" }

if ($Uninstall) {
    # (never installed from a copy like this one: Frame Home itself puts SteamVR's environment back)
    & $ssh $HostName "if [ -f frame-home/.install/frame/install.sh ]; then sh frame-home/.install/frame/install.sh uninstall; else frame-home/bin/frame-home --uninstall; fi"
} else {
    Say "copying Frame Home"
    & $ssh $HostName "rm -rf frame-home/.install/frame"
    # Relative source from its own folder: scp would read "C:\..." as a host named C.
    Push-Location $PSScriptRoot
    try { & $scp -q -r payload "${HostName}:frame-home/.install/frame" } finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { throw "Copying to the headset failed" }
    foreach ($f in $Environments) {
        if (-not (Test-Path $f)) { Say "no such file: $f"; continue }
        $name = Frame-Name $f
        Say "copying $([IO.Path]::GetFileName($f)) -> $name"
        Push-Location (Split-Path -Parent (Resolve-Path $f))
        try { & $scp -q ([IO.Path]::GetFileName($f)) "${HostName}:frame-home/envs/$name.part" } finally { Pop-Location }
        & $ssh $HostName "mv -f frame-home/envs/$name.part frame-home/envs/$name"
    }
    & $ssh $HostName "sed -i 's/\r`$//' frame-home/.install/frame/install.sh; sh frame-home/.install/frame/install.sh"
    if ($LASTEXITCODE -ne 0) { throw "The installer on the headset failed (see above)" }
    # The first environment given is the one shown (a bundle's are only known once the headset unpacks it: skipped).
    $first = $Environments | Where-Object { $_ -match "\.fenv$" } | Select-Object -First 1
    if ($first -and (Test-Path $first)) {
        $name = Frame-Name $first
        & $ssh $HostName "printf '%s\n' '$name' > frame-home/current"
    }
}
Say "done"
