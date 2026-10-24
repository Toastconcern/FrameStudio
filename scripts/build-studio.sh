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
#   all                                    every target this computer can build
# Output: dist/FrameStudio-<target>.zip (.tar.gz on Linux).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DIST="$ROOT/dist"
BUILD="$ROOT/build"
FRAME_PAYLOAD_DIR=${FRAME_HOME_PAYLOAD_DIR:-}
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
if [ -z "$FRAME_PAYLOAD_DIR" ]; then
    for candidate in "$BUILD/frame-home/payload" "$ROOT/build/frame-home/payload"; do
        if [ -d "$candidate" ]; then FRAME_PAYLOAD_DIR=$candidate; break; fi
    done
    [ -n "$FRAME_PAYLOAD_DIR" ] || FRAME_PAYLOAD_DIR="$ROOT/frame/payload"
fi
mkdir -p "$DIST" "$BUILD"
say() { echo "build-studio: $*"; }
say "using Frame Home payload: $FRAME_PAYLOAD_DIR"

if [ "$TARGETS" = " all" ]; then
    case "$(uname -s)" in
        Darwin) TARGETS="macos-arm64 macos-x64" ;;
        *) case "$(uname -m)" in
               aarch64|arm64) TARGETS="linux-arm64" ;;
               *) TARGETS="linux-x64 linux-x86" ;;
           esac
           [ -x "${LLVM_MINGW:-$HOME/.local/opt/llvm-mingw}/bin/x86_64-w64-mingw32-clang" ] && TARGETS="$TARGETS windows-x64 windows-x86 windows-arm64" ;;
    esac
fi

# configure + build one target; $1 target, then extra cmake arguments
build() {
    target=$1; shift
    dir="$BUILD/$target"
    say "building $target"
    "$CMAKE" -S "$ROOT/studio" -B "$dir" -DCMAKE_BUILD_TYPE=Release \
        "-DFRAME_HOME_PAYLOAD_DIR=$FRAME_PAYLOAD_DIR" "$@" > "$dir.configure.log" 2>&1 || {
        tail -20 "$dir.configure.log"; say "$target: configure failed (full log: $dir.configure.log)"; exit 1; }
    "$CMAKE" --build "$dir" --target frame-studio --parallel
}

# Frame Home for SteamVR on the computer itself (Linux x64), built from the top-level project into
# $BUILD/<target>-home. $1 target, then extra cmake arguments.
build_pc_home() {
    target=$1; shift
    dir="$BUILD/$target-home"
    say "building Frame Home for $target's SteamVR"
    "$CMAKE" -S "$ROOT" -B "$dir" -DCMAKE_BUILD_TYPE=Release -DFRAME_HOME_PC=ON "$@" > "$dir.configure.log" 2>&1 || {
        tail -20 "$dir.configure.log"; say "$target: Frame Home configure failed (full log: $dir.configure.log)"; exit 1; }
    "$CMAKE" --build "$dir" --target frame-home frame-switcher --parallel
}

# pc/ beside Studio: Frame Home for this computer's SteamVR and its installer (Studio's Headset tab runs it), the Steam
# Frame's way — the Vulkan compositor layer draws the home in Aurora's place, installed with the headset's own
# install.sh (Linux only).
stage_pc() {
    target=$1 stage=$2 home="$BUILD/$1-home"
    case "$target" in
        linux-x64)   mkdir -p "$stage/pc/bin" "$stage/pc/layer"
                     cp "$home/frame-home" "$home/frame-switcher" "$stage/pc/bin/"
                     ${CXX:-g++} -O2 -shared -fPIC -std=c++17 -fvisibility=hidden -I"$ROOT/third_party/vulkan-headers/include" -I"$ROOT/src" \
                        "$ROOT/layer/frame_home_layer.cpp" -o "$stage/pc/layer/libframe_home_layer.so" -lpthread
                     cp "$FRAME_PAYLOAD_DIR/layer/env_ps.spv" "$stage/pc/layer/"
                     cp "$FRAME_PAYLOAD_DIR/install.sh" "$stage/pc/" && chmod +x "$stage/pc/install.sh" "$stage"/pc/bin/* ;;
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

for t in $TARGETS; do
    case "$t" in
        linux-x64) build "$t" -G Ninja && build_pc_home "$t" -G Ninja && package "$t" ;;
        linux-arm64) build "$t" -G Ninja && package "$t" ;;
        linux-x86) build "$t" -G Ninja -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 -DCMAKE_SYSTEM_PROCESSOR=i686 \
                       -DCMAKE_LIBRARY_ARCHITECTURE=i386-linux-gnu && package "$t" ;;
        windows-x64) build "$t" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw.cmake" -DWINDOWS_ARCH=x86_64 && package "$t" ;;
        windows-x86) build "$t" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw.cmake" -DWINDOWS_ARCH=i686 && package "$t" ;;
        windows-arm64) build "$t" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/llvm-mingw.cmake" -DWINDOWS_ARCH=aarch64 && package "$t" ;;
        macos-arm64) build "$t" -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 && package "$t" ;;
        macos-x64) build "$t" -G Ninja -DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 && package "$t" ;;
        *) say "unknown target: $t (see --help)"; exit 1 ;;
    esac
done
