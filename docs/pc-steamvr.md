# Frame Home on a PC (SteamVR)

Frame Home also runs on a PC with SteamVR, exactly the way it runs on the Steam Frame: a small layer inside SteamVR's
compositor draws your home where it would draw Aurora, so your home *is* the SteamVR environment — it shows at boot,
during game loads and behind the Steam menu, with the controllers and passthrough, never a separate app that launches
and hands off. The **Environments** panel in the Steam menu switches homes, as on the headset. It works with any
headset SteamVR drives, including a Steam Frame streaming from the PC.

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

Both platforms work the Steam Frame's way: a layer inside SteamVR's compositor draws your home in Aurora's place, and
`frame-home --system` (which the Environments panel starts and keeps running) renders it. Because the swap happens
inside the compositor, your home shows wherever Aurora would — at boot, during game loads, behind the Steam menu,
with the controllers and passthrough — and steps aside for a game the instant it opens, coming back when it quits.

**Windows.** SteamVR's compositor is Direct3D 11, so the layer is a proxy `d3d11.dll` placed beside the compositor
(`SteamVR\bin\win64`). It wraps the compositor's device, finds Aurora's own draws by their shader bytecode and draws
the home over them; every other Direct3D call passes straight through to the real `d3d11.dll`, kept beside it as
`fh_d3d11real.dll`. Frame Home renders in Vulkan and shares each frame to the compositor as a Direct3D 11 texture.

**Linux.** The same, as a Vulkan layer in the compositor (`layer/frame_home_layer.cpp`), since the Linux compositor
is Vulkan. (Linux PCs are supported but haven't been tested on a desktop yet.)

If the layer ever can't draw the home — Frame Home isn't running, or has stopped for more than half a second — the
compositor draws Aurora as usual, so Frame Home stopping or crashing never blanks the headset. The layer turns itself
off if the compositor starts twice without a healthy frame, and `%LOCALAPPDATA%\Frame Home\layer\off` turns it off by
hand. The installer saves the SteamVR settings it changes (`enableHomeApp`, `environmentMode`) and puts them back,
and removing the two files from `SteamVR\bin\win64` takes the layer off entirely.

## Files

| Where | What |
|---|---|
| `%LOCALAPPDATA%\Frame Home\bin` | `frame-home.exe`, `frame-switcher.exe` |
| `%LOCALAPPDATA%\Frame Home\current` | the environment shown |
| `%LOCALAPPDATA%\Frame Home\settings.json` | Frame Home's settings (the panel's Settings page) |
| `%LOCALAPPDATA%\Frame Home\logs` | logs (`frame-home.log` the renderer, `frame-layer.log` the compositor layer) |
| `%LOCALAPPDATA%\Frame Home\layer` | the layer's off switch and crash guard; its folder marks the layer installed |
| `Documents\Frame Studio\Environments` | the environments |
| `SteamVR\bin\win64\d3d11.dll`, `fh_d3d11real.dll` | the compositor layer and the real Direct3D 11 beside it |

On Linux everything is in `~/frame-home`, as on the headset.

## Troubleshooting

- **Aurora still shows after installing:** the layer loads when the compositor starts, so restart SteamVR (or your
  headset) once after installing. If it still shows, see `frame-layer.log`.
- **Nothing in the Steam menu:** the panel starts with SteamVR. In SteamVR's settings, **Startup / Shutdown > Choose
  Startup Overlay Apps** should list **Environments** switched on.
- **Black world:** Frame Home needs a GPU with Vulkan 1.2. Its log is `%LOCALAPPDATA%\Frame Home\logs\frame-home.log`.
- **Turn it off without uninstalling:** make a file `%LOCALAPPDATA%\Frame Home\layer\off` (the layer checks every
  second and draws Aurora); delete it to turn the home back on.
