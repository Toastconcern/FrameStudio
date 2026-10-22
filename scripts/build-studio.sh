#!/bin/sh
# Builds Frame Studio for one or more computers and packages each into dist/, ready to hand out.
#
#   scripts/build-studio.sh TARGET... [--build-dir DIR]
#
# Targets (each needs the matching compiler; GitHub Actions builds all of them: .github/workflows/build.yml):
#   linux-x64  linux-x86  linux-arm64      on Linux: the native compiler (x86: gcc-multilib / g++-multilib); X11
#                                          headers (libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev)
#   windows-x64  windows-x86  windows-arm64  on Linux with llvm-mingw (cmake/llvm-mingw.cmake; $LLVM_MINGW)
#                                          (on Windows itself: studio\build_windows.bat x64|x86|arm64)
#   macos-arm64  macos-x64                 on a Mac with Xcode's command line tools; MoltenVK is bundled into the
#                                          app from $MOLTENVK (a libMoltenVK.dylib) or Homebrew's molten-vk
#   frame-home                             the Steam Frame installer on its own (no Studio needed): the headset
#                                          files from frame/payload and the install scripts
#   all                                    every target this computer can build
# Output: dist/FrameStudio-<target>.zip (.tar.gz on Linux) and dist/FrameHome-installer.zip.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST="$ROOT/dist"
BUILD="$ROOT/build"
CMAKE=${CMAKE:-cmake}
TARGETS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD=$2; shift 2 ;;
        -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
        *) TARGETS="$TARGETS $1"; shift ;;
    esac
done
[ -n "$TARGETS" ] || { sed -n '2,19p' "$0"; exit 1; }
mkdir -p "$DIST" "$BUILD"
say() { echo "build-studio: $*"; }

if [ "$TARGETS" = " all" ]; then
    case "$(uname -s)" in
        Darwin) TARGETS="macos-arm64 macos-x64 frame-home" ;;
        *) case "$(uname -m)" in
               aarch64|arm64) TARGETS="linux-arm64 frame-home" ;;
               *) TARGETS="linux-x64 linux-x86 frame-home" ;;
           esac
           [ -x "${LLVM_MINGW:-$HOME/.local/opt/llvm-mingw}/bin/x86_64-w64-mingw32-clang" ] && TARGETS="$TARGETS windows-x64 windows-x86 windows-arm64" ;;
    esac
fi

# configure + build one target; $1 target, then extra cmake arguments
build() {
    target=$1; shift
    dir="$BUILD/$target"
    say "building $target"
    "$CMAKE" -S "$ROOT/studio" -B "$dir" -DCMAKE_BUILD_TYPE=Release "$@" > "$dir.configure.log" 2>&1 || {
        tail -20 "$dir.configure.log"; say "$target: configure failed (full log: $dir.configure.log)"; exit 1; }
    "$CMAKE" --build "$dir" --target frame-studio --parallel
}

# Frame Home for SteamVR on the computer itself (Windows x64, Linux x64), built from the top-level project into
# $BUILD/<target>-home. $1 target, then extra cmake arguments.
build_pc_home() {
    target=$1; shift
    dir="$BUILD/$target-home"
    say "building Frame Home for $target's SteamVR"
    "$CMAKE" -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE=Release -DFRAME_HOME_PC=ON "$@" > "$dir.configure.log" 2>&1 || {
        tail -20 "$dir.configure.log"; say "$target: Frame Home configure failed (full log: $dir.configure.log)"; exit 1; }
    "$CMAKE" --build "$dir" --target frame-home frame-switcher $([ "$target" = windows-x64 ] && echo d3d11.dll) --parallel
}

# pc/ beside Studio: Frame Home for this computer's SteamVR and its installer (Studio's Headset tab runs it). Both
# platforms the Steam Frame's way now — the compositor layer draws the home in Aurora's place. Windows: the layer is
# a proxy d3d11.dll (install-frame-home-pc.ps1 puts it beside SteamVR's compositor). Linux: the Vulkan layer, with
# the headset's own install.sh.
stage_pc() {
    target=$1 stage=$2 home="$BUILD/$1-home"
    case "$target" in
        windows-x64) mkdir -p "$stage/pc"
                     cp "$home/frame-home.exe" "$home/frame-switcher.exe" "$home/d3d11.dll" "$stage/pc/"
                     cp "$ROOT/pc-streaming/install-frame-home-pc.ps1" "$ROOT/pc-streaming/Install Frame Home on this PC.bat" \
                        "$ROOT/pc-streaming/Remove Frame Home from this PC.bat" "$stage/pc/" ;;
        linux-x64)   mkdir -p "$stage/pc/bin" "$stage/pc/layer"
                     cp "$home/frame-home" "$home/frame-switcher" "$stage/pc/bin/"
                     ${CXX:-g++} -O2 -shared -fPIC -std=c++17 -fvisibility=hidden -I"$ROOT/third_party/vulkan-headers/include" -I"$ROOT/src" \
                        "$ROOT/layer/frame_home_layer.cpp" -o "$stage/pc/layer/libframe_home_layer.so" -lpthread
                     cp "$ROOT/frame/payload/layer/env_ps.spv" "$stage/pc/layer/"
                     cp "$ROOT/frame/payload/install.sh" "$stage/pc/" && chmod +x "$stage/pc/install.sh" "$stage"/pc/bin/* ;;
    esac
}

# dist/FrameStudio-<target>: the program and, beside it, frame/ (the headset side Studio installs) and pc/ (Frame Home
# for this computer's SteamVR, where there is one)
package() {
    target=$1 dir="$BUILD/$1" name="FrameStudio-$1"
    stage="$BUILD/stage/$name"
    rm -rf "$stage" && mkdir -p "$stage"
    cp "$ROOT/LICENSE" "$stage/LICENSE.txt"
    case "$target" in
        windows-*) cp "$dir/Frame Studio.exe" "$stage/" && cp -R "$dir/frame" "$stage/frame"
                   stage_pc "$target" "$stage"
                   (cd "$BUILD/stage" && rm -f "$DIST/$name.zip" && zip -qr "$DIST/$name.zip" "$name") ;;
        linux-*)   cp "$dir/Frame Studio" "$stage/" && cp -R "$dir/frame" "$stage/frame"
                   stage_pc "$target" "$stage"
                   tar -czf "$DIST/$name.tar.gz" -C "$BUILD/stage" "$name" ;;
        macos-*)   cp -R "$dir/Frame Studio.app" "$stage/"
                   bundle_moltenvk "$stage/Frame Studio.app"
                   (cd "$BUILD/stage" && rm -f "$DIST/$name.zip" && ditto -c -k --keepParent "$name" "$DIST/$name.zip") ;;
    esac
    say "$target -> dist/$(ls "$DIST" | grep "^$name\." | head -1)"
}

# MoltenVK (Vulkan on Metal) inside the app: Contents/Frameworks/libMoltenVK.dylib, which Studio loads itself when
# the Mac has no Vulkan loader installed.
bundle_moltenvk() {
    app=$1
    lib=${MOLTENVK:-}
    [ -n "$lib" ] || { command -v brew > /dev/null && lib="$(brew --prefix molten-vk 2>/dev/null)/lib/libMoltenVK.dylib"; } || true
    if [ -n "$lib" ] && [ -f "$lib" ]; then
        mkdir -p "$app/Contents/Frameworks" && cp "$lib" "$app/Contents/Frameworks/"
    else
        say "warning: no MoltenVK found (set MOLTENVK or: brew install molten-vk): the app needs the Vulkan SDK installed to run"
    fi
}

# dist/FrameHome-installer.zip: Frame Home for the Steam Frame, installed without Frame Studio
package_frame_home() {
    stage="$BUILD/stage/FrameHome-installer"
    rm -rf "$stage" && mkdir -p "$stage"
    cp -R "$ROOT/frame/payload" "$stage/payload"
    cp "$ROOT/LICENSE" "$stage/LICENSE.txt"
    cp "$ROOT/frame/install-frame-home.sh" "$ROOT/frame/install-frame-home.ps1" "$ROOT/frame/Install Frame Home.bat" "$ROOT/frame/README-frame-home.txt" "$stage/"
    chmod +x "$stage/install-frame-home.sh" "$stage/payload/install.sh" "$stage"/payload/bin/* 2>/dev/null || true
    (cd "$BUILD/stage" && rm -f "$DIST/FrameHome-installer.zip" && zip -qr "$DIST/FrameHome-installer.zip" FrameHome-installer)
    say "frame-home -> dist/FrameHome-installer.zip"
}

for t in $TARGETS; do
    case "$t" in
        linux-x64) build "$t" -G Ninja && build_pc_home "$t" -G Ninja && package "$t" ;;
        linux-arm64) build "$t" -G Ninja && package "$t" ;;
        linux-x86) build "$t" -G Ninja -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_SYSTEM_PROCESSOR=i686 \
                       -DCMAKE_LIBRARY_ARCHITECTURE=i386-linux-gnu && package "$t" ;;
        windows-x64) build "$t" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw.cmake" -DWINDOWS_ARCH=x86_64 &&
                     build_pc_home "$t" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw.cmake" -DWINDOWS_ARCH=x86_64 && package "$t" ;;
        windows-x86) build "$t" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw.cmake" -DWINDOWS_ARCH=i686 && package "$t" ;;
        windows-arm64) build "$t" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw.cmake" -DWINDOWS_ARCH=aarch64 && package "$t" ;;
        macos-arm64) build "$t" -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 && package "$t" ;;
        macos-x64) build "$t" -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 && package "$t" ;;
        frame-home) package_frame_home ;;
        *) say "unknown target: $t (see --help)"; exit 1 ;;
    esac
done
