# Building from source

You only need this page if you want to change the code or build a version that isn't on the Releases page. Ready
builds for every computer are on GitHub (see [getting-started.md](getting-started.md#download)).

There are two parts:

- **Frame Studio** builds on Windows, Linux and macOS (and cross-builds for Windows from Linux).
- **The headset side** (Frame Home, the Environments panel, `fenv-tool` and the compositor layer) builds on the
  Steam Frame itself. A ready-built copy is in the repository, in `frame/payload/`, so you only rebuild it when you
  change that code.

Everything Studio needs is in `third_party/`. Vulkan is loaded when Studio starts, so you don't need the Vulkan SDK
to build it. All builds use CMake 3.16 or later and Ninja.

## Frame Studio on Windows

Prerequisites: Visual Studio 2022 (any edition) with the **Desktop development with C++** workload. It includes
CMake and Ninja.

1. Open a Command Prompt in the repository folder.
2. Run:

   ```bat
   studio\build_windows.bat x64
   ```

   Use `x86` for 32-bit Windows or `arm64` for Windows on ARM instead of `x64`.
3. The program is `%LOCALAPPDATA%\FrameStudio\build-x64\Frame Studio.exe` (the folder ends in your architecture).
   The headset files are copied beside it, in `frame\`.

To build into another folder, give it as the second argument: `studio\build_windows.bat x64 C:\builds\studio`.

For `x86` or `arm64`, Visual Studio needs the matching C++ tools. For ARM64, add "MSVC ... ARM64/ARM64EC build
tools" in the Visual Studio Installer.

## Frame Studio with scripts/build-studio.sh (Linux, macOS)

```sh
scripts/build-studio.sh TARGET... [--build-dir DIR]
```

Each target is built and packed into `dist/`. `scripts/build-studio.sh --help` lists them.

| Target | Build on | Output |
|---|---|---|
| `linux-x64`, `linux-arm64` | Linux on that kind of machine | `dist/FrameStudio-linux-<arch>.tar.gz` |
| `linux-x86` | 64-bit Linux with `gcc-multilib g++-multilib` | `dist/FrameStudio-linux-x86.tar.gz` |
| `windows-x64`, `windows-x86`, `windows-arm64` | Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | `dist/FrameStudio-windows-<arch>.zip` |
| `macos-arm64`, `macos-x64` | A Mac with Xcode's command line tools | `dist/FrameStudio-macos-<arch>.zip` |
| `frame-home` | Anywhere | `dist/FrameHome-installer.zip` (the headset files and install scripts, no Studio) |
| `all` | Anywhere | Every target this computer can build |

`all` builds: on a Mac, both macOS targets and `frame-home`; on 64-bit x86 Linux, `linux-x64`, `linux-x86` and
`frame-home`; on ARM Linux, `linux-arm64` and `frame-home`. On Linux it adds the three Windows targets when it finds
llvm-mingw.

Build folders go in `build/<target>` unless you pass `--build-dir`.

### Linux prerequisites

Debian and Ubuntu package names:

```sh
sudo apt install build-essential cmake ninja-build \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
```

For `linux-x86`, also install `gcc-multilib g++-multilib`.

Then:

```sh
scripts/build-studio.sh linux-x64
```

### Windows from Linux (llvm-mingw)

1. Download an llvm-mingw release and unpack it, by default to `~/.local/opt/llvm-mingw`. Or set `LLVM_MINGW` to
   where it is.
2. Run:

   ```sh
   scripts/build-studio.sh windows-x64 windows-x86 windows-arm64
   ```

The toolchain file is `cmake/llvm-mingw.cmake`.

### macOS prerequisites

1. Install Xcode's command line tools: `xcode-select --install`.
2. Install Ninja and MoltenVK (Vulkan on Metal) with Homebrew:

   ```sh
   brew install ninja molten-vk
   ```

3. Build:

   ```sh
   scripts/build-studio.sh macos-arm64 macos-x64
   ```

The script copies MoltenVK into the app (`Contents/Frameworks/libMoltenVK.dylib`). To use another copy, set
`MOLTENVK` to the path of a `libMoltenVK.dylib`. Without MoltenVK the app needs the Vulkan SDK installed to run.

The Apple Silicon build runs on macOS 11 and later, the Intel build on macOS 10.15 and later.

## Continuous builds (GitHub Actions)

The workflow is `.github/workflows/build.yml`.

| When | What happens |
|---|---|
| Every push, to any branch or tag, and every pull request | All targets build: Linux x64, x86, arm64; Windows x64, x86, arm64 (MSVC); macOS arm64 and x64; the Frame Home installer. Each is kept as an artifact of the run. |
| A push to `main` | The **Latest build** pre-release is replaced with the new builds. Builds that worked are published even if another platform failed. |
| A tag starting with `v` (for example `v1.0`) | A release with that name is published, with all builds. It is only made when every build succeeded. |
| By hand | **Actions > build > Run workflow** |

A newer push to the same branch cancels a run that is still going.

To publish a version:

```sh
git tag v1.0
git push --tags
```

To download a build from any run:

1. Open the repository on GitHub and choose **Actions**.
2. Click the run (each push is one).
3. Scroll down to **Artifacts** and download the one you need. You must be signed in.

## The headset side

Frame Home (`frame-home`), the Environments panel (`frame-switcher`), `fenv-tool` and the compositor layer are built
on the Steam Frame itself, against SteamVR's own OpenVR library (`/opt/steamvr/bin/linuxarm64`, set by
`OPENVR_LIB_DIR`). You need a C++17 compiler, CMake, Ninja and the Vulkan headers on the headset.

1. Copy the repository to the headset and log in: `ssh frame`.
2. Build the programs with the top-level `CMakeLists.txt`:

   ```sh
   cd ~/frame-env
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

   This makes `build/frame-home`, `build/frame-switcher` and `build/fenv-tool`.
3. Build the compositor layer (it is not part of CMake):

   ```sh
   mkdir -p build/layer
   g++ -O2 -shared -fPIC -std=c++17 -fvisibility=hidden layer/frame_home_layer.cpp -o build/layer/libframe_home_layer.so -lpthread
   ```

4. Compile the layer's shader `layer/env_ps.frag` to SPIR-V, for example with glslc:

   ```sh
   glslc layer/env_ps.frag -o build/layer/env_ps.spv
   ```

### Refreshing frame/payload

`frame/payload/` is the ready-built copy that every Studio build and the installer carry. After building on the
headset, copy the results into it:

| From (on the headset) | To (in the repository) |
|---|---|
| `build/frame-home`, `build/frame-switcher`, `build/fenv-tool` | `frame/payload/bin/` |
| `build/layer/libframe_home_layer.so`, `build/layer/env_ps.spv` | `frame/payload/layer/` |

`frame/payload/install.sh` is the installer itself; it lives in the repository and needs no build.

Then record which source they were built from, and commit the new payload together with the code that changed:

```sh
sh scripts/frame-payload.sh stamp
```

`frame/payload/SOURCE` holds a hash of everything the headset side is built from (`src/`, `third_party/`, `layer/`,
`CMakeLists.txt`). `sh scripts/frame-payload.sh check` tells you whether the payload is still current. The release
build runs it too and publishes nothing while the payload is older than the source: a Studio beside an older Frame
Home saves environments the headset can't open. Studio also asks the headset (`fenv-tool version`) before
**Send to Frame** and says when its Frame Home needs updating.

## Repository layout

| Path | What it holds |
|---|---|
| `studio/` | Frame Studio: `studio.cpp` (the editor), `platform*.{h,cpp}` (Windows, Linux, macOS), `frame_pacer.h`, `build_windows.bat`, its `CMakeLists.txt` |
| `src/` | The shared engine: renderer (`hsl_renderer`), scene loading and baking, `.fenv`, walkable area, Frame Home (`main.cpp`), the Environments panel (`switcher.cpp`), `fenv-tool` |
| `layer/` | The Vulkan layer inside SteamVR's compositor that shows Frame Home in place of its environment, and its shader |
| `frame/` | The headset payload (`payload/`) and the stand-alone installers |
| `scripts/` | `build-studio.sh` |
| `cmake/` | The llvm-mingw toolchain file |
| `tests_loco/` | Movement tests |
| `tools/` | Test and measuring tools (headless renders, walk tests, shader sources and generators) |
| `third_party/` | Vendored libraries, each with its own license |
| `docs/` | These guides |
| `CMakeLists.txt` | The headset-side build |
| `.github/workflows/build.yml` | The CI |
