# Building from source

You only need this page if you want to change the code or build a version that isn't on the Releases page. Ready
builds for every computer are on GitHub (see [getting-started.md](getting-started.md#download)).

There are two build surfaces:

- **Frame Studio** builds on Windows, Linux and macOS (and cross-builds for Windows from Linux).
- **Frame Home** (the renderer, Environments panel, `fenv-tool` and compositor layer) is built by GitHub Actions on
   ARM64 Linux, and can also be built locally on the Steam Frame.

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

   Download the `FrameHome-payload` artifact from an Actions run, extract it, and set `FRAME_HOME_PAYLOAD_DIR` to
   its payload directory before running the batch file, or [build it manually](#frame-home-payload).

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
The script uses `build/frame-home/payload` when present. Set `FRAME_HOME_PAYLOAD_DIR` to use a payload from another
location.

| Target | Build on | Output |
|---|---|---|
| `linux-x64`, `linux-arm64` | Linux on that kind of machine | `dist/FrameStudio-linux-<arch>.tar.gz` |
| `linux-x86` | 64-bit Linux with `gcc-multilib g++-multilib` | `dist/FrameStudio-linux-x86.tar.gz` |
| `windows-x64`, `windows-x86`, `windows-arm64` | Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | `dist/FrameStudio-windows-<arch>.zip` |
| `macos-arm64`, `macos-x64` | A Mac with Xcode's command line tools | `dist/FrameStudio-macos-<arch>.zip` |
| `all` | Anywhere | Every target this computer can build |

`all` builds the targets supported by this computer. On Linux it adds the three Windows targets when it finds
llvm-mingw. The standalone Frame Home installer is built separately by GitHub Actions.

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

The `.github/workflows/build.yml` workflow builds both products and publishes their assets together. There is one
rolling `latest` release and one shared `v*` tag namespace; each release contains the Studio packages and
`FrameHome-installer.zip`.

| When | What happens |
|---|---|
| Every push and pull request | Studio builds for Linux, Windows and macOS; Frame Home is built on ARM64 and bundled into Studio packages. Successful outputs are kept as run artifacts. |
| A push to `main` | The **Latest build** pre-release is replaced with Studio builds and the standalone Frame Home installer. |
| A tag starting with `v` (for example `v1.0`) | One version release is published with both Studio packages and the Frame Home installer. |
| By hand | **Actions > build > Run workflow** |

A newer push to the same branch cancels a run that is still going.

To publish versions:

```sh
git tag v1.0
git push --tags
```

To download a build from any run:

1. Open the repository on GitHub and choose **Actions**.
2. Click the run (each push is one).
3. Scroll down to **Artifacts** and download the one you need. You must be signed in.

## Frame Home payload

The release payload is built from source by GitHub Actions on ARM64 Linux, currently pinned to glibc 2.39 for compatibility with SteamOS stable/beta. The same artifact is used by Studio packages and the standalone installer.

To build and test on the Steam Frame, run:

```sh
sh scripts/build-frame-home.sh
```

This uses the headset's native OpenVR library and Vulkan SDK, normally at `/opt/steamvr/bin/linuxarm64`, and requires
CMake, Ninja, a C/C++ compiler, `glslc`, `zip`, and Python 3. Set `OPENVR_LIB_DIR` if SteamVR is installed elsewhere. The
payload, installer ZIP, and build manifest are written under `build/frame-home/`. This mode accepts the headset's
installed glibc version, including Preview's 2.43; those local outputs are for development and are not the release
build for stable/beta.

CI selects release mode explicitly with `sh scripts/build-frame-home.sh --ci` (or `FRAME_HOME_BUILD_MODE=ci`). It
requires ARM64 Linux with glibc 2.39 and GCC 13, then checks the ELF architecture and GLIBC symbol ceiling before
packaging. The build manifest records compiler/tool versions, dynamic dependencies, and
GLIBC/GLIBCXX/CXXABI requirements in `build/frame-home/build-info.txt`; CI retains it as a run artifact. Studio can
bundle a locally built or downloaded payload by setting `FRAME_HOME_PAYLOAD_DIR` to that payload directory. The
installer ZIP is checked against a Frame Home-only file allowlist before it is published.

Studio checks the headset's `fenv-tool version` before **Send to Frame** and warns when the headset cannot open the
scene format Studio would save.

### Attestations

Frame Home installer and Studio release archives carry GitHub build-provenance attestations. With GitHub CLI, verify
an asset from the repository root:

```sh
gh attestation verify FrameHome-installer.zip --repo Toastconcern/FrameStudio
gh attestation verify FrameStudio-linux-x64.tar.gz --repo Toastconcern/FrameStudio
```

The attestation identifies the repository, workflow, source ref and commit that produced the archive. It does not
replace runtime signature verification by the installer or updater.

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
