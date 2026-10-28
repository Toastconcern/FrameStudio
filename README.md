# Frame Studio and Frame Home

Turn Meta Quest homes and 3D models into native environments for the Steam Frame.

| | Runs on | What it does |
|---|---|---|
| **Frame Home** | The Steam Frame | Draws your environment in SteamVR's compositor in place of its own: at boot, behind the Steam menu and while games load |
| **Frame Studio** | Windows, Linux, macOS, the Steam Frame | Opens Meta homes (HSL, Clay `.opa`, glTF), `.fenv` environments and models (glTF, FBX, OBJ), shows them with the same renderer the headset runs, and lets you edit, save and send them to the headset |

**Help and news:** the [Frame Studio Discord](https://discord.gg/yJDn59ErgK) for help, bug reports, feature requests,
new releases and environments people have made. What changed in each version: [CHANGELOG.md](CHANGELOG.md).

## Contents

- [Quick start](#quick-start)
- [Install only Frame Home](#install-only-frame-home)
- [Downloads](#downloads)
- [Features](#features)
- [Guides](#guides)
- [Building from source](#building-from-source)
- [License](#license)

## Quick start

1. **Download** Frame Studio for your computer from the Releases page and unpack it.
   See [Getting started](docs/getting-started.md).
2. **Prepare the headset** once: developer mode, SSH and a key. See [Preparing the Steam Frame](docs/headset-setup.md).
3. **Install Frame Home**: in Studio, **Headset > Install on Steam Frame...**, then **Install Frame Home**.
   See [Installing Frame Home](docs/install-frame-home.md).
4. **Open a home**: **File > Open...** and pick a Meta home `.apk`, a `.fenv` or a model.
5. **Send it**: **Headset > Send to Frame**. Put the headset on: you are standing in it.

## Install only Frame Home

You don't need Frame Studio to run Frame Home. On the Steam Frame:

1. Switch to **Desktop Mode** and open **Konsole**.
2. Run:

   ```sh
   curl -fsSL https://raw.githubusercontent.com/Toastconcern/FrameStudio/main/frame/install-frame-home-online.sh | bash
   ```

   This downloads Frame Home from the **Latest build** release, checks it and installs it. No SSH or computer needed.
3. Put your `.fenv` environments in `~/frame-home/envs`.
4. Switch back to Game Mode. Your environment shows at boot and behind the Steam menu; switch between environments
   in the **Environments** panel.

To install from a computer instead, download `FrameHome-installer.zip` and run **Install Frame Home.bat** (Windows)
or `install-frame-home.sh` (Linux, macOS). That way needs SSH on the headset. Every method, plus updating and
uninstalling: [Installing Frame Home](docs/install-frame-home.md).

## Downloads

Every release on the Releases page (**Latest build**, or a numbered `v*` version) contains Frame Studio for each
computer below and the matching Frame Home payload.

| Computer | File |
|---|---|
| Windows, 64-bit PCs | `FrameStudio-windows-x64.zip` |
| Windows on ARM (Snapdragon, Surface Pro X...) | `FrameStudio-windows-arm64.zip` |
| Linux, 64-bit PCs | `FrameStudio-linux-x64.tar.gz` |
| Linux on ARM (Raspberry Pi 5, ARM laptops) | `FrameStudio-linux-arm64.tar.gz` |
| Steam Frame (SteamOS ARM64) | `FrameStudio-linux-arm64.tar.gz` |
| Mac with Apple Silicon (M1 and later) | `FrameStudio-macos-arm64.zip` |
| Frame Home only, installed from any computer | `FrameHome-installer.zip` |

**Requirements.** Studio needs a GPU with Vulkan 1.2: any recent NVIDIA, AMD or Intel driver on Windows and Linux.
On a Mac, Vulkan runs on Metal through MoltenVK, which comes inside the app. 32-bit Windows and Linux and Intel Macs
have no ready-made download, but Studio can still be [built from source](docs/building.md) for them.

**Updates.** Studio updates from its top bar, and Frame Home from the Environments panel on the headset. Both follow
the same release channel.

## Features

### Frame Home, on the headset

- Takes SteamVR's environment slot natively: no app, no library entry, controllers and passthrough still work.
- An **Environments** panel in the Steam menu: switch homes live, delete them, change settings, debug views.
- Walk, teleport, snap or smooth turn, sit in seats, gravity zones, comfort vignette.
- Built for 120 fps: baked environments, eye-tracked foveation, auto resolution, per-world tuned settings, and a
  thermal governor that keeps the headset cool.
- Safe: a compositor layer that steps aside if Frame Home stops, and a one-file off switch.

### Frame Studio, on your computer

- **Open** Meta homes (HSL, Clay, glTF), `.fenv` environments and Blender or Unity models; pair rooms with vistas.
- **Edit**: move, rotate, scale, hide and delete objects, with undo and auto-saved versions. A navigation gizmo in
  the viewport corner orbits the view or snaps it to an axis.
- **Shape the walkable area**: auto-generated surfaces, drawn floors, spawns, seats, hotspots, gravity zones.
- **Set the mood**: a background sound, a picture, and per-world settings you can set or calculate.
- **Save and send**: baked `.fenv` files that leave out what can't be seen, sent to the headset in one click, or
  packed with others into one `.fbundle` to share or install together ([bundles](docs/bundles.md)).
- **Check it**: debug views (wireframe, meshes, GPU cost, overdraw), a home report, and the headset's settings and
  debug page from your desk.
- Themes, from dark to light (**Window > Theme**).

## Guides

| Guide | What it covers |
|---|---|
| [Getting started](docs/getting-started.md) | Requirements, downloads, first launch, a 5-minute walkthrough |
| [Preparing the Steam Frame](docs/headset-setup.md) | Developer mode, SSH, keys, the `frame` address |
| [Installing Frame Home](docs/install-frame-home.md) | Three ways to install, updating, uninstalling, the off switch |
| [Using Frame Home](docs/using-frame-home.md) | The Environments panel, moving around, every setting |
| [Frame Studio guide](docs/studio-guide.md) | The Studio window, tools and panels |
| [Environments and .fenv](docs/environments-and-fenv.md) | The format, `fenv-tool`, per-world settings and tuning |
| [Bundles](docs/bundles.md) | Several environments in one `.fbundle` |
| [Performance](docs/performance.md) | Reaching 120 fps, checking it, fixing stutter |
| [Troubleshooting](docs/troubleshooting.md) | Symptoms and fixes, logs, the self-test |
| [Building from source](docs/building.md) | Building on each system, CI, and Frame Home payload builds |

## Building from source

Everything Studio needs is in `third_party/`; you need CMake 3.16+ and Ninja.

| On | Command |
|---|---|
| Windows (Visual Studio 2022 with C++) | `studio\build_windows.bat x64` (or `x86`, `arm64`) |
| Linux, macOS | `scripts/build-studio.sh <target>...` or `scripts/build-studio.sh all` |

Frame Home binaries are built in GitHub Actions and are not checked into `frame/payload/`. Prerequisites, CI and the
headset side: [docs/building.md](docs/building.md).

### Repository layout

| Path | What it holds |
|---|---|
| `studio/` | Frame Studio: `studio.cpp` (the editor), `platform*.{h,cpp}` (Windows, Linux, macOS), `frame_pacer.h` |
| `src/` | Shared engine: renderer (`hsl_renderer`), scene loading and baking, `.fenv`, walkable area, Frame Home (`main.cpp`), the Environments panel (`switcher.cpp`), `fenv-tool` |
| `layer/` | The Vulkan layer inside SteamVR's compositor that shows Frame Home in place of its environment |
| `frame/` | Frame Home installer source and stand-alone installer scripts |
| `scripts/`, `cmake/` | Build script and the llvm-mingw toolchain file |
| `tools/`, `tests_loco/` | Test and measuring tools, movement tests |
| `third_party/` | Vendored libraries (each with its own license) |
| `docs/` | The guides |

## License

Frame Studio and Frame Home are licensed under the **GNU Affero General Public License v3.0**: see [LICENSE](LICENSE).
You may use, study, change and share them; if you share a changed version, or run one for others over a network,
its source must be available under the same license.

Vendored libraries in `third_party/` keep their own licenses (in each folder); Studio's interface font, Inter, is under
the SIL Open Font License (`third_party/inter/LICENSE.txt`). The home loaders in `third_party/qhe` started from Quest
Home Editor and keep its MIT license for the original code; see [third_party/qhe/LICENSE.md](third_party/qhe/LICENSE.md).

Neither license covers Meta's homes or other assets: Frame Studio only works with files you already have, and no
Meta content is included in this repository or its downloads.
