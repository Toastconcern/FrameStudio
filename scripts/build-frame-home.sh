#!/bin/sh
# Build the Steam Frame payload and its standalone installer on an ARM64 glibc 2.39 runner.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${FRAME_HOME_OUTPUT_DIR:-"$ROOT/build/frame-home"}
PAYLOAD="$OUT/payload"
MODE=${1:-${FRAME_HOME_BUILD_MODE:-local}}
case "$MODE" in
    ci|--ci)
        MODE=ci
        BUILD=${FRAME_HOME_BUILD_DIR:-"$ROOT/build/frame-home-cmake-ci"}
        CC=${CC:-gcc-13}
        CXX=${CXX:-g++-13}
        ;;
    local|--local)
        MODE=local
        BUILD=${FRAME_HOME_BUILD_DIR:-"$ROOT/build/frame-home-cmake-local"}
        CC=${CC:-gcc}
        CXX=${CXX:-g++}
        ;;
    *) echo "Usage: $0 [--local|--ci]" >&2; exit 2 ;;
esac
export CC CXX

[ "$(uname -m)" = aarch64 ] || { echo "Frame Home requires an ARM64 runner" >&2; exit 1; }
if [ "$MODE" = ci ] && [ "$(getconf GNU_LIBC_VERSION)" != "glibc 2.39" ]; then
    echo "Release builds require glibc 2.39 (found: $(getconf GNU_LIBC_VERSION))" >&2
    exit 1
fi
for tool in cmake ninja "$CC" "$CXX" glslc readelf zip python3; do
    command -v "$tool" >/dev/null 2>&1 || { echo "Missing build tool: $tool" >&2; exit 1; }
done
if [ "$MODE" = ci ]; then
    "$CXX" --version | grep -q '13\.' || { echo "Release builds require GCC 13" >&2; exit 1; }
fi

if [ "$MODE" = local ]; then
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFRAME_HOME_PC=OFF \
        "-DOPENVR_LIB_DIR=${OPENVR_LIB_DIR:-/opt/steamvr/bin/linuxarm64}"
else
    cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFRAME_HOME_PC=ON
fi
cmake --build "$BUILD" --target frame-home frame-switcher fenv-tool --parallel

rm -rf "$PAYLOAD" "$OUT/FrameHome-installer-stage" "$OUT/FrameHome-installer.zip"
mkdir -p "$PAYLOAD/bin" "$PAYLOAD/layer"
cp "$BUILD/frame-home" "$BUILD/frame-switcher" "$BUILD/fenv-tool" "$PAYLOAD/bin/"
"$CXX" -O2 -shared -fPIC -std=c++17 -fvisibility=hidden \
    -I"$ROOT/third_party/vulkan-headers/include" -I"$ROOT/src" \
    "$ROOT/layer/frame_home_layer.cpp" -o "$PAYLOAD/layer/libframe_home_layer.so" -lpthread
glslc "$ROOT/layer/env_ps.frag" -o "$PAYLOAD/layer/env_ps.spv"
cp "$ROOT/frame/payload/install.sh" "$PAYLOAD/install.sh"
chmod +x "$PAYLOAD/install.sh" "$PAYLOAD/bin/"*

for binary in "$PAYLOAD/bin/"* "$PAYLOAD/layer/libframe_home_layer.so"; do
    readelf -h "$binary" | grep -q 'Machine:.*AArch64' || {
        echo "Not an AArch64 ELF: $binary" >&2; exit 1;
    }
    highest=$(readelf --version-info "$binary" | grep -oE 'GLIBC_[0-9]+(\.[0-9]+)+' | sort -Vu | tail -n 1 || true)
    if [ "$MODE" = ci ] && [ -n "$highest" ] && [ "$(printf '%s\n' "$highest" GLIBC_2.39 | sort -V | tail -n 1)" != GLIBC_2.39 ]; then
        echo "$binary requires $highest, newer than the glibc 2.39 baseline" >&2
        exit 1
    fi
done

STAGE="$OUT/FrameHome-installer-stage/FrameHome-installer"
mkdir -p "$STAGE"
cp -R "$PAYLOAD" "$STAGE/payload"
cp "$ROOT/LICENSE" "$STAGE/LICENSE.txt"
cp "$ROOT/frame/install-frame-home.sh" "$ROOT/frame/install-frame-home.ps1" \
    "$ROOT/frame/Install Frame Home.bat" "$ROOT/frame/README-frame-home.txt" "$STAGE/"
chmod +x "$STAGE/install-frame-home.sh"
(cd "$OUT/FrameHome-installer-stage" && zip -qr "$OUT/FrameHome-installer.zip" FrameHome-installer)
rm -rf "$OUT/FrameHome-installer-stage"
python3 - "$OUT/FrameHome-installer.zip" <<'PY'
import sys
import zipfile

required = {
    "FrameHome-installer/payload/install.sh",
    "FrameHome-installer/payload/bin/frame-home",
    "FrameHome-installer/payload/bin/frame-switcher",
    "FrameHome-installer/payload/bin/fenv-tool",
    "FrameHome-installer/payload/layer/libframe_home_layer.so",
    "FrameHome-installer/payload/layer/env_ps.spv",
}
allowed_files = {
    "FrameHome-installer/LICENSE.txt",
    "FrameHome-installer/Install Frame Home.bat",
    "FrameHome-installer/install-frame-home.ps1",
    "FrameHome-installer/install-frame-home.sh",
    "FrameHome-installer/README-frame-home.txt",
}
with zipfile.ZipFile(sys.argv[1]) as archive:
    names = {
        info.filename.rstrip("/")
        for info in archive.infolist()
        if not info.is_dir()
    }
    missing = required - names
    unexpected = names - required - allowed_files
    if missing or unexpected:
        raise SystemExit(f"Invalid Frame Home installer: missing={sorted(missing)}, unexpected={sorted(unexpected)}")
PY
{
    printf 'mode=%s\n' "$MODE"
    printf 'architecture=%s\n' "$(uname -m)"
    printf 'libc=%s\n' "$(getconf GNU_LIBC_VERSION)"
    printf 'c_compiler=%s\n' "$("$CC" --version | head -n 1)"
    printf 'cxx_compiler=%s\n' "$("$CXX" --version | head -n 1)"
    printf 'cmake=%s\n' "$(cmake --version | head -n 1)"
    printf 'ninja=%s\n' "$(ninja --version)"
    printf 'glslc=%s\n' "$(glslc --version | head -n 1)"
} > "$OUT/build-info.txt"
for binary in "$PAYLOAD/bin/"* "$PAYLOAD/layer/libframe_home_layer.so"; do
    printf '\n[%s]\n' "${binary#"$PAYLOAD/"}" >> "$OUT/build-info.txt"
    readelf -d "$binary" | grep -E 'NEEDED|RPATH|RUNPATH' >> "$OUT/build-info.txt" || true
    readelf --version-info "$binary" | grep -oE '(GLIBC|GLIBCXX|CXXABI)_[0-9]+(\.[0-9]+)+' | sort -Vu | tr '\n' ' ' >> "$OUT/build-info.txt" || true
    printf '\n' >> "$OUT/build-info.txt"
done
echo "Built $OUT/payload, $OUT/FrameHome-installer.zip, and $OUT/build-info.txt"
