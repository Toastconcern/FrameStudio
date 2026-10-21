# Installing Frame Home

Frame Home is the part that runs on the Steam Frame. There are three ways to install it. All three do the same
thing on the headset: they copy the files and run the same installer (`install.sh`).

Before you start, the headset needs developer mode and SSH: see [headset-setup.md](headset-setup.md). The third way
(on the headset itself) needs no SSH.

## Way 1: from Frame Studio

1. Open the **Headset** tab in Properties (on the right), or choose **Headset > Install on Steam Frame...**, which opens it.
2. In **Headset**, type the headset's address: an SSH alias such as `frame`, or `steamos@<address>`, for example
   `steamos@192.168.1.50`. Studio remembers it.
3. Under **Environments to install**, choose what to copy along:
   - **The open home** (if one is open). It is saved as a `.fenv` first if it isn't one already.
   - **Add .fenv...** adds more environment files.
   - With nothing chosen, only Frame Home is installed. Environments already on the headset stay.
4. Click **Install Frame Home**. The tab shows each step and, at the end, "Installed".

Environments already on the headset with the same name and size are not copied again.

**Restore SteamVR's Environment** in the same tab uninstalls (see [Uninstalling](#uninstalling)).

Studio connects without asking for passwords, so it needs an SSH key (see [headset-setup.md](headset-setup.md)).

## Way 2: with FrameHome-installer.zip, from any computer

Download `FrameHome-installer.zip` from the Releases page (see [getting-started.md](getting-started.md#download))
and unpack it. Inside are:

| File | For |
|---|---|
| `Install Frame Home.bat` | Windows: double-click it |
| `install-frame-home.ps1` | Windows: the script the .bat runs (PowerShell) |
| `install-frame-home.sh` | Linux and macOS |
| `payload/` | The headset files (programs, compositor layer, `install.sh`) |
| `README-frame-home.txt` | A short version of this page |

These installers can use a key or a password. With a password, SSH asks for it several times (once per step).

### Windows

1. Double-click **Install Frame Home.bat**.
2. It asks for the **Steam Frame address**. Press Enter to keep the one shown (`frame`, or the address you last used
   in Studio), or type another, such as `steamos@192.168.1.50`.
3. Wait for "Frame Home is installed.", then press a key to close the window.

To copy environments at the same time, drag one or more `.fenv` files onto **Install Frame Home.bat**. The first
one becomes the environment shown.

Windows needs an SSH client: "OpenSSH Client" under **Settings > Apps > Optional features**, or Git for Windows.

From PowerShell you can also run the script directly:

```powershell
powershell -ExecutionPolicy Bypass -File install-frame-home.ps1 -HostName steamos@192.168.1.50 C:\path\to\home.fenv
```

| Option | Meaning |
|---|---|
| `-HostName HOST` | The headset's SSH address. Default `steamos@frame`. A name without `user@` gets `steamos@` added. |
| `-Uninstall` | Give the environment back to SteamVR (files stay). |
| `file.fenv ...` | Environments to copy. The first is the one shown. |

### Linux and macOS

```sh
sh install-frame-home.sh --host steamos@192.168.1.50 my-home.fenv another.fenv
```

| Option | Meaning |
|---|---|
| `--host HOST` | The headset's SSH address. Default `steamos@frame`. A name without `user@` gets `steamos@` added. |
| `--uninstall` | Give the environment back to SteamVR (files stay). |
| `file.fenv ...` | Environments to copy. The first is the one shown. |
| `-h`, `--help` | Show the usage. |

Copied environments get a headset-friendly name: lower case, spaces become dashes, and only letters, digits, `.`,
`_` and `-` are kept. `My Home.fenv` becomes `my-home.fenv`.

## Way 3: directly on the headset

1. Switch the headset to Desktop Mode.
2. Copy the unpacked `FrameHome-installer` folder to the headset (USB stick, network share, or a download in the
   headset's browser).
3. Open a terminal (Konsole) in that folder.
4. Run:

   ```sh
   sh payload/install.sh
   ```

5. Put `.fenv` files in `~/frame-home/envs`. The Environments panel lists them.

## What gets installed where

Everything goes into your home folder on the headset. Nothing is changed under `/opt` or `/usr`.

| Path on the headset | What it is |
|---|---|
| `~/frame-home/bin/` | The programs: `frame-home` (draws your home), `frame-switcher` (the Environments panel), `fenv-tool` (converts and tunes environments) |
| `~/frame-home/envs/` | Your environments (`.fenv`, or Meta home `.apk` files) |
| `~/frame-home/envs/.thumbs/` | Thumbnails for the panel, made on the headset |
| `~/frame-home/layer/` | The compositor layer (`libframe_home_layer.so`, `env_ps.spv`) and its log |
| `~/frame-home/current` | The name of the environment shown now |
| `~/frame-home/settings.json` | Your settings (see [using-frame-home.md](using-frame-home.md)) |
| `~/frame-home/stats.json` | How Frame Home is running, for the panel and Studio to show |
| `~/frame-home/logs/` | Logs, the environment report, and pictures saved from the Debug page |
| `~/frame-home/.install/frame/` | The copy of the installer that Studio and the installers upload |
| `~/.local/share/vulkan/implicit_layer.d/frame_home_layer.json` | Registers the compositor layer with Vulkan |
| `~/.cache/frame-home/` | Unpacked environments and other caches (safe to delete; rebuilt when needed) |

SteamVR's config (`~/.config/openvr/`) gets two app manifests registered and the Environments panel set to start
with SteamVR.

## How Frame Home takes SteamVR's environment slot

SteamVR draws its own environment inside the compositor (the program that puts everything on the displays). Frame
Home leaves that in place and swaps only the picture:

1. A small **compositor layer** loads into SteamVR's compositor. Wherever the compositor would draw its own
   environment, the layer draws the newest frame Frame Home rendered instead.
2. **frame-home** renders your home in the background, at the display's frame rate.
3. **frame-switcher**, the Environments panel, starts with SteamVR. It keeps `frame-home` running and restarts it if
   it stops.

Because the compositor still does the drawing, your home shows at boot, behind the Steam menu and during game loads,
the controllers stay visible, and passthrough still works.

The layer is careful:

- It is active only inside SteamVR's compositor. Everywhere else it does nothing.
- If Frame Home has not sent a frame for half a second (stopped or crashed), SteamVR's own environment shows.
- If the compositor stops twice in a row before the layer got going, the layer turns itself off (it creates the
  `off` file, see below).

## Updating

Install again, any of the three ways. The installer:

- replaces the programs and the layer (a running copy keeps its old file until it restarts),
- keeps your environments, settings and choice of environment,
- restarts the Environments panel and Frame Home so the new version takes over at once. If the layer changed, it
  also restarts SteamVR's compositor, which comes straight back.

If SteamVR is not running, the installer writes the registration into SteamVR's config files. Frame Home then takes
over the next time SteamVR starts.

Studio's download carries its own copy of the headset files. Installing from an older Studio installs that older
Frame Home.

## Uninstalling

| From | Do this |
|---|---|
| Studio | **Headset > Install on Steam Frame...**, then **Restore SteamVR's Environment** |
| Windows | `powershell -ExecutionPolicy Bypass -File install-frame-home.ps1 -Uninstall` |
| Linux, macOS | `sh install-frame-home.sh --uninstall` |
| On the headset | `sh payload/install.sh uninstall` |

This gives the environment back to SteamVR. SteamVR's own environment returns once SteamVR restarts.
Your environments and the programs in `~/frame-home` stay. To remove everything, delete `~/frame-home` afterwards.

## The emergency off switch

If anything goes wrong, create this file on the headset:

```sh
touch ~/frame-home/layer/off
```

The layer checks for it every second and steps aside: SteamVR's own environment shows (at the latest after
SteamVR's next start). Nothing else is changed.

To turn Frame Home back on, do one of these:

- In the Environments panel, press **Start Frame Home** or **Turn Frame Home On**. This deletes the file and starts
  Frame Home.
- Delete the file yourself: `rm ~/frame-home/layer/off`. If your home does not come back, restart SteamVR.
- Install again. The installer removes the file.

If the layer turned itself off after two failed starts, the panel says "Off: it failed to start twice".

## Backups

The installer does not make backups. Before you try something new, you can save your settings and programs with one
command on the headset:

```sh
tar -czf ~/frame-home-backup-$(date +%Y%m%d-%H%M%S).tgz -C ~ --exclude=frame-home/envs frame-home
```

This leaves out `envs/`, which is large. Your environments are usually also on your computer (in Studio's My
Environments folder). To restore, unpack the archive into your home folder (`tar -xzf <file> -C ~`) and restart
SteamVR.
