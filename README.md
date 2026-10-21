# Frame Studio and Frame Home

**Frame Home** makes your homes the Steam Frame's environment. Meta Quest homes (HSL, Clay `.opa`, glTF) and models
become Frame-native `.fenv` environments, drawn in SteamVR's compositor in place of its own environment: at boot,
behind the Steam menu and while games load. **Frame Studio** is the desktop editor for them, on Windows, Linux and
macOS. It opens Meta homes, `.fenv` environments and models (glTF, FBX, OBJ), shows them with the same renderer the
headset runs, and lets you edit, save and send them to the headset.

## Quick start

1. **Download** Frame Studio for your computer from the Releases page (**Latest build**, or a version release) and
   unpack it. See [Getting started](docs/getting-started.md).
2. **Prepare the headset** once: developer mode, SSH and a key. See [Preparing the Steam Frame](docs/headset-setup.md).
3. **Install Frame Home**: in Studio, **Headset > Install on Steam Frame...**, then **Install Frame Home**.
   See [Installing Frame Home](docs/install-frame-home.md).
4. **Open a home**: **File > Open...** and pick a Meta home `.apk`, a `.fenv` or a model.
5. **Send it**: **Headset > Send to Frame**. Put the headset on: you are standing in it.

## Downloads

Every push builds everything. The newest build of `main` is always on the Releases page as **Latest build**; tagged
versions (`v1.0`, ...) get their own release. Builds of other branches are under Actions (open a run, then
Artifacts).

| Computer | Frame Studio |
|---|---|
| Windows, 64-bit PCs | `FrameStudio-windows-x64.zip` |
| Windows, 32-bit | `FrameStudio-windows-x86.zip` |
| Windows on ARM (Snapdragon, Surface Pro X...) | `FrameStudio-windows-arm64.zip` |
| Linux, 64-bit PCs | `FrameStudio-linux-x64.tar.gz` |
| Linux, 32-bit | `FrameStudio-linux-x86.tar.gz` |
| Linux on ARM (Raspberry Pi 5, ARM laptops, the Steam Frame's desktop) | `FrameStudio-linux-arm64.tar.gz` |
| Mac with Apple Silicon (M1 and later) | `FrameStudio-macos-arm64.zip` |
| Mac with Intel | `FrameStudio-macos-x64.zip` |
| Frame Home only, installed from any computer | `FrameHome-installer.zip` |
| Frame Home for SteamVR on a Windows PC, without Studio | `FrameHome-PC-windows-x64.zip` (Studio's Windows x64 and Linux x64 downloads include it) |

Studio needs a GPU with Vulkan 1.2: any recent NVIDIA, AMD or Intel driver on Windows and Linux. On a Mac, Vulkan
runs on Metal through MoltenVK, which comes inside the app. The 32-bit builds have at most 4 GB to work in, so the
very largest homes may not fit.

## Highlights

**Frame Home (on the headset)**

- Takes SteamVR's environment slot natively: no app, no library entry, controllers and passthrough still work.
- An **Environments** panel in the Steam menu: switch homes live, delete them, change settings, debug views.
- Walk, teleport, snap or smooth turn, sit in seats, gravity zones, comfort vignette.
- Built for 120 fps: baked environments, eye-tracked foveation, auto resolution, per-world tuned settings, and a
  thermal governor that keeps the headset cool.
- Safe: a compositor layer that steps aside if Frame Home stops, and a one-file off switch.

**On a PC with SteamVR**

- The same Frame Home as SteamVR's environment on Windows and Linux, switched from the Steam menu, showing what you
  save in Studio ([guide](docs/pc-steamvr.md)).

**Frame Studio (on your computer)**

- Opens Meta homes (HSL, Clay, glTF), `.fenv` environments and Blender or Unity models; pairs rooms with vistas.
- Move, rotate, scale, hide and delete objects, with undo and auto-saved versions.
- Shape the walkable area: auto-generated surfaces, drawn floors, spawns, seats, hotspots, wall spots, gravity zones.
- Add a background sound and a picture; set and calculate per-world settings.
- Saves baked `.fenv` files and leaves out what can't be seen; sends them to the headset in one click.
- Packs several environments into one `.fbundle` to share or install together ([bundles](docs/bundles.md)).
- Debug views (wireframe, meshes, GPU cost, overdraw), a home report, and the headset's settings and debug page
  from your desk.

## Guides

| Guide | What it covers |
|---|---|
| [Getting started](docs/getting-started.md) | Requirements, downloads, first launch, a 5-minute walkthrough |
| [Preparing the Steam Frame](docs/headset-setup.md) | Developer mode, SSH, keys, the `frame` address |
| [Installing Frame Home](docs/install-frame-home.md) | Three ways to install, updating, uninstalling, the off switch |
| [Using Frame Home](docs/using-frame-home.md) | The Environments panel, moving around, every setting |
| [Environments and .fenv](docs/environments-and-fenv.md) | The format, `fenv-tool`, per-world settings and tuning |
| [Performance](docs/performance.md) | Reaching 120 fps, checking it, fixing stutter |
| [Frame Studio guide](docs/studio-guide.md) | The Studio window, tools and panels |
| [Bundles](docs/bundles.md) | Several environments in one `.fbundle` |
| [Frame Home on a PC](docs/pc-steamvr.md) | Your homes as SteamVR's environment on a Windows or Linux PC |
| [Building from source](docs/building.md) | Building on each system, CI, the headset side |
| [Troubleshooting](docs/troubleshooting.md) | Symptoms and fixes, logs, the self-test |

## Building from source

Everything Studio needs is in `third_party/`; you need CMake 3.16+ and Ninja. In short:

| On | Command |
|---|---|
| Windows (Visual Studio 2022 with C++) | `studio\build_windows.bat x64` (or `x86`, `arm64`) |
| Linux, macOS | `scripts/build-studio.sh <target>...` or `scripts/build-studio.sh all` |

The headset side is built on the Steam Frame; a ready-built copy is in `frame/payload/`. Full details, prerequisites
and the CI: [docs/building.md](docs/building.md).

## Repository layout

| Path | What it holds |
|---|---|
| `studio/` | Frame Studio: `studio.cpp` (the editor), `platform*.{h,cpp}` (Windows, Linux, macOS), `frame_pacer.h` |
| `src/` | Shared engine: renderer (`hsl_renderer`), scene loading and baking, `.fenv`, walkable area, Frame Home (`main.cpp`), the Environments panel (`switcher.cpp`), `fenv-tool` |
| `layer/` | The Vulkan layer inside SteamVR's compositor that shows Frame Home in place of its environment |
| `frame/` | The headset payload and the stand-alone installers |
| `scripts/`, `cmake/` | Build script and the llvm-mingw toolchain file |
| `tools/`, `tests_loco/` | Test and measuring tools, movement tests |
| `third_party/` | Vendored libraries (each with its own license) |
| `docs/` | The guides |

## License

Frame Studio and Frame Home are licensed under the **GNU Affero General Public License v3.0**: see [LICENSE](LICENSE).
You may use, study, change and share them; if you share a changed version, or run one for others over a network,
its source must be available under the same license.

Vendored libraries in `third_party/` keep their own licenses (in each folder); Studio's interface font, Inter, is under
the SIL Open Font License (`third_party/inter/LICENSE.txt`). The home loaders in `third_party/qhe`
started from Quest Home Editor and keep its MIT license for the original code; see
[third_party/qhe/LICENSE.md](third_party/qhe/LICENSE.md).

Neither license covers Meta's homes or other assets: Frame Studio only works with files you already have, and no
Meta content is included in this repository or its downloads.
