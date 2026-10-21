# Frame Studio and Frame Home

**Frame Home** makes your homes the Steam Frame's environment: Meta Quest homes (HSL, Clay `.opa`, glTF) and models,
converted into Frame-native `.fenv` environments and drawn in SteamVR's compositor in place of its own.

**Frame Studio** is the desktop editor for them. It opens Meta homes, `.fenv` environments and models (glTF, FBX,
OBJ), shows them with the same renderer the headset runs, and lets you move, hide and delete objects, shape the walkable
area, add sounds and pictures, save environments and install them on the headset.

## Downloads

Every push builds everything (Actions → the latest run → Artifacts); tagged versions are on the Releases page.

| Computer | Frame Studio |
|---|---|
| Windows, 64-bit PCs | `FrameStudio-windows-x64.zip` |
| Windows, 32-bit | `FrameStudio-windows-x86.zip` |
| Windows on ARM (Snapdragon, Surface Pro X…) | `FrameStudio-windows-arm64.zip` |
| Linux, 64-bit PCs | `FrameStudio-linux-x64.tar.gz` |
| Linux, 32-bit | `FrameStudio-linux-x86.tar.gz` |
| Linux on ARM (Raspberry Pi 5, ARM laptops, the Steam Frame's desktop) | `FrameStudio-linux-arm64.tar.gz` |
| Mac with Apple Silicon (M1 and later) | `FrameStudio-macos-arm64.zip` |
| Mac with Intel | `FrameStudio-macos-x64.zip` |

Studio needs a GPU with Vulkan 1.2: any recent NVIDIA, AMD or Intel driver on Windows and Linux; on a Mac, Vulkan runs
on Metal through MoltenVK, which comes inside the app. The 32-bit builds have at most 4 GB to work in: the very
largest homes may not fit.

**Frame Home without Studio:** `FrameHome-installer.zip` installs it on the headset from any computer (or on the
headset itself); see `frame/README-frame-home.txt` inside it.

## Building

Everything Studio needs is in `third_party/` (Vulkan is loaded at runtime). CMake 3.16+ and Ninja.

| On | Command | Builds |
|---|---|---|
| Windows (Visual Studio 2022 with C++) | `studio\build_windows.bat x64` (or `x86`, `arm64`) | Windows |
| Linux | `scripts/build-studio.sh linux-x64` (or `linux-x86`, `linux-arm64` on an ARM machine) | Linux |
| Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | `scripts/build-studio.sh windows-x64 windows-x86 windows-arm64` | Windows, cross |
| macOS (Xcode command line tools, `brew install ninja molten-vk`) | `scripts/build-studio.sh macos-arm64 macos-x64` | macOS |
| anywhere | `scripts/build-studio.sh frame-home` | the Frame Home installer |
| anywhere it can | `scripts/build-studio.sh all` | everything this computer can build |

Linux builds need the X11 headers (`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev`) and, for
`linux-x86`, `gcc-multilib g++-multilib`. Packages land in `dist/`.

The headset side (`frame-home`, `frame-switcher`, `fenv-tool`, the compositor layer) is built on the Steam Frame
itself against SteamVR's own OpenVR library: the top-level `CMakeLists.txt`, then
`g++ -O2 -shared -fPIC -std=c++17 layer/frame_home_layer.cpp -o build/layer/libframe_home_layer.so`. Its ready-built
copy is `frame/payload/`, which every Studio build and the installer carry.

## Layout

| | |
|---|---|
| `studio/` | Frame Studio: `studio.cpp` (the editor), `platform*.{h,cpp}` (Windows / Linux / macOS), `frame_pacer.h` |
| `src/` | shared engine: renderer (`hsl_renderer`), scene loading and baking, `.fenv`, walkable area, Frame Home (`main.cpp`), `fenv-tool` |
| `layer/` | the Vulkan layer inside SteamVR's compositor that shows Frame Home in place of its environment |
| `frame/` | the headset payload and the stand-alone installers |
| `third_party/` | vendored libraries (each with its own license) |
| `tools/` | test and measuring tools (headless renders, walk tests, shader generators) |
