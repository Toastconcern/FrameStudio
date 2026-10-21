# Frame Home on a PC (SteamVR)

Frame Home also runs on a PC with SteamVR: the same renderer, homes and settings as on the Steam Frame, as SteamVR's
environment whenever no game is open. The **Environments** panel in the Steam menu switches homes, as on the headset.
It works with any headset SteamVR drives, including a Steam Frame streaming from the PC.

On a PC, Frame Home shows the environments in Frame Studio's **My Environments** (`Documents\Frame Studio\Environments`):
save something in Studio and it's in VR straight away. Bundles (`.fbundle`) dropped there are unpacked by the panel.

## Install

**From Frame Studio:** **Properties > Headset**, section **This PC**: **Install Frame Home on This PC**. The same button
updates it after you update Studio; **Remove from This PC** takes it off.

**Without Studio (Windows):** download `FrameHome-PC-windows-x64.zip` from the
[latest build](https://github.com/Toastconcern/FrameStudio/releases), unzip it, and double-click
**Install Frame Home on this PC.bat** (or **Remove Frame Home from this PC.bat**). From a terminal:

```powershell
powershell -ExecutionPolicy Bypass -File install-frame-home-pc.ps1              # install or update
powershell -ExecutionPolicy Bypass -File install-frame-home-pc.ps1 -Uninstall   # SteamVR's own environment back
```

SteamVR can be running or not: running, the change happens at once; not running, it's written to SteamVR's settings
and takes effect when SteamVR starts.

## How it works

**Windows.** Frame Home becomes SteamVR's *home app*: SteamVR starts it whenever no game is open, and brings it back
when a game quits. For the moments before it draws (SteamVR starting, a game closing) SteamVR shows its classic
environment instead of Aurora, with its background set to a 360-degree picture of your current home taken from its
spawn. So you see your home, not Aurora. The panel makes a new picture whenever you switch homes.

**Linux.** Exactly the Steam Frame's way: a small Vulkan layer in SteamVR's compositor draws Frame Home in Aurora's
place, and `frame-home --system` renders it. (Linux PCs are supported but haven't been tested on a desktop yet.)

The installer saves the SteamVR settings it changes (`enableHomeApp`, `environmentMode`, `background` and its
projection) and puts them back exactly when you remove Frame Home.

## Files

| Where | What |
|---|---|
| `%LOCALAPPDATA%\Frame Home\bin` | `frame-home.exe`, `frame-switcher.exe` |
| `%LOCALAPPDATA%\Frame Home\current` | the environment shown |
| `%LOCALAPPDATA%\Frame Home\settings.json` | Frame Home's settings (the panel's Settings page) |
| `%LOCALAPPDATA%\Frame Home\logs` | logs |
| `%LOCALAPPDATA%\Frame Home\background` | the picture SteamVR shows while Frame Home starts |
| `Documents\Frame Studio\Environments` | the environments |

On Linux everything is in `~/frame-home`, as on the headset.

## Troubleshooting

- **Aurora still shows after installing:** SteamVR was running with its settings window open; restart SteamVR.
- **Nothing in the Steam menu:** the panel starts with SteamVR. In SteamVR's settings, **Startup / Shutdown > Choose
  Startup Overlay Apps** should list **Environments** switched on.
- **Black world:** Frame Home needs a GPU with Vulkan 1.2. Its log is `%LOCALAPPDATA%\Frame Home\logs\frame-home.log`.
