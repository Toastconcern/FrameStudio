cmake_minimum_required(VERSION 3.16)
project(frame_home LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(TP "${CMAKE_SOURCE_DIR}/third_party")

# The Steam Frame build (SteamOS, arm64) links SteamVR's own OpenVR library and the system's Vulkan. A PC build
# (Windows, or x86-64 Linux) has to run on any PC with SteamVR: it finds SteamVR's OpenVR library at run time
# (src/openvr_loader.cpp) and loads Vulkan the same way (volk), so it depends on neither at build time.
if(WIN32 OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(FRAME_HOME_PC_DEFAULT ON)
else()
    set(FRAME_HOME_PC_DEFAULT OFF)
endif()
option(FRAME_HOME_PC "Build Frame Home for a PC running SteamVR (OpenVR and Vulkan found at run time)" ${FRAME_HOME_PC_DEFAULT})
if(FRAME_HOME_PC)
    add_library(volk STATIC "${TP}/volk/volk.c")
    target_include_directories(volk PUBLIC "${TP}/volk" "${TP}/vulkan-headers/include")
    if(NOT WIN32)
        target_link_libraries(volk PUBLIC dl)
    endif()
    add_library(openvr-loader STATIC src/openvr_loader.cpp)
    target_include_directories(openvr-loader PUBLIC "${TP}/openvr")
    target_compile_definitions(openvr-loader PUBLIC OPENVR_BUILD_STATIC)
    if(WIN32)
        target_link_libraries(openvr-loader PUBLIC shell32 ole32)
    else()
        target_link_libraries(openvr-loader PUBLIC dl)
    endif()
    set(FH_VULKAN volk)
    set(FH_OPENVR openvr-loader)
else()
    # SteamVR ships the OpenVR client library; link against the runtime's own copy.
    set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "Directory containing libopenvr_api.so")
    find_library(OPENVR_LIB NAMES openvr_api PATHS "${OPENVR_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
    find_package(Vulkan REQUIRED)
    set(FH_VULKAN Vulkan::Vulkan)
    set(FH_OPENVR "${OPENVR_LIB}")
endif()

# ── Editor loader stack (Quest Home Editor: ASMH/HSTF/RENDMESH/MATL/RENDTXTR/RENDSHAD) ──
add_library(miniz STATIC "${TP}/miniz/miniz.c" "${TP}/miniz/miniz_tdef.c" "${TP}/miniz/miniz_tinfl.c" "${TP}/miniz/miniz_zip.c")
target_include_directories(miniz PUBLIC "${TP}/miniz")
# APKs are signed and intact; re-checksumming every extracted asset was ~20% of load time.
target_compile_definitions(miniz PRIVATE MINIZ_DISABLE_ZIP_READER_CRC32_CHECKS)
add_library(smolv STATIC "${TP}/smolv/smolv.cpp")
target_include_directories(smolv PUBLIC "${TP}/smolv")

# astcenc: decoder for the homes' ASTC textures (LDR + HDR lightmaps).
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_DECOMPRESSOR OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
    set(ASTCENC_LIB astcenc-neon-static)
else()
    set(ASTCENC_ISA_SSE41 ON CACHE BOOL "" FORCE)
    set(ASTCENC_LIB astcenc-sse4.1-static)
endif()
add_subdirectory("${TP}/astcenc" astcenc EXCLUDE_FROM_ALL)

# LZMA SDK (public domain): the .fenv scene coder (src/fenv_lzma.cpp), with its threaded match finder.
add_library(lzma STATIC "${TP}/lzma/LzmaEnc.c" "${TP}/lzma/LzmaDec.c" "${TP}/lzma/LzFind.c" "${TP}/lzma/LzFindOpt.c"
    "${TP}/lzma/LzFindMt.c" "${TP}/lzma/Threads.c" "${TP}/lzma/CpuArch.c")
target_include_directories(lzma PUBLIC "${TP}/lzma")

# zstd 1.5.6 (BSD), decompression only: version 4 .spz splat files (src/splat_import.cpp). Single-threaded, plain C
# (no amd64 assembly), so it builds the same everywhere.
add_library(zstd STATIC "${TP}/zstd/common/debug.c" "${TP}/zstd/common/entropy_common.c"
    "${TP}/zstd/common/error_private.c" "${TP}/zstd/common/fse_decompress.c" "${TP}/zstd/common/xxhash.c"
    "${TP}/zstd/common/zstd_common.c" "${TP}/zstd/decompress/huf_decompress.c" "${TP}/zstd/decompress/zstd_ddict.c"
    "${TP}/zstd/decompress/zstd_decompress.c" "${TP}/zstd/decompress/zstd_decompress_block.c")
target_include_directories(zstd PUBLIC "${TP}/zstd")
target_compile_definitions(zstd PRIVATE ZSTD_DISABLE_ASM)

# The home loader: HSL / Clay / glTF homes, .fenv environments (baked scenes), Blender / Unity models.
add_library(frame-scene STATIC
    src/hsl_scene.cpp
    src/scene_bake.cpp
    src/anim_bake.cpp
    src/fenv_lzma.cpp
    src/model_import.cpp
    src/splat_import.cpp
    src/spv_opt.cpp
    "${TP}/ufbx/ufbx.c"
    "${TP}/qhe/cook/hzanim_acl.cpp"
    "${TP}/stb/stb_image_impl.c")
target_include_directories(frame-scene PUBLIC
    src
    "${TP}/stb"
    "${TP}/qhe"
    "${TP}/miniz"
    "${TP}/cgltf"
    "${TP}/ufbx"
    "${TP}/acl/includes"
    "${TP}/rtm/includes"
    "${TP}/astcenc/Source")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties("${TP}/qhe/cook/hzanim_acl.cpp" PROPERTIES COMPILE_OPTIONS "-flax-vector-conversions")
endif()
target_link_libraries(frame-scene PUBLIC miniz lzma zstd smolv ${ASTCENC_LIB} pthread)
# The SPIRV-Tools optimizer for src/spv_opt.h (FH_SPV_OPT=1 at run time). Off: on the Frame it made no shader any
# cheaper (Turnip's compiler already does what it does), and it is a large library to build everywhere.
option(FRAME_SPV_OPT "Build in the SPIRV-Tools shader optimizer (third_party/spirv-tools)" OFF)
if(FRAME_SPV_OPT)
    add_subdirectory("${TP}/spirv-tools" spirv-tools EXCLUDE_FROM_ALL)
    target_link_libraries(frame-scene PUBLIC spirv-tools-opt)
    target_compile_definitions(frame-scene PUBLIC FH_HAVE_SPV_OPT)
endif()
# libdeflate when the system has it (the Frame, CI): APK scene.zips inflate several times faster than with miniz. Its
# static library first, so a release build doesn't need it installed on the headset.
find_library(LIBDEFLATE_LIB NAMES libdeflate.a deflate)
find_path(LIBDEFLATE_INC libdeflate.h)
if(LIBDEFLATE_LIB AND LIBDEFLATE_INC)
    target_compile_definitions(frame-scene PUBLIC QHE_HAVE_LIBDEFLATE)
    target_include_directories(frame-scene PUBLIC "${LIBDEFLATE_INC}")
    target_link_libraries(frame-scene PUBLIC "${LIBDEFLATE_LIB}")
endif()

# Ogg Opus ambience (Clay homes' _BACKGROUND_LOOP.ogg, or a sound added in Frame Studio): Xiph libopus + libogg +
# opusfile (BSD), decode only — as Frame Studio has it, so the headset plays every sound the preview does.
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${TP}/opus" opus EXCLUDE_FROM_ALL)
add_library(ogg STATIC "${TP}/ogg/src/bitwise.c" "${TP}/ogg/src/framing.c")
target_include_directories(ogg PUBLIC "${TP}/ogg/include")
add_library(opusfile STATIC "${TP}/opusfile/src/info.c" "${TP}/opusfile/src/internal.c"
    "${TP}/opusfile/src/opusfile.c" "${TP}/opusfile/src/stream.c")
target_include_directories(opusfile PUBLIC "${TP}/opusfile/include" PRIVATE "${TP}/opusfile/src")
target_compile_definitions(opusfile PRIVATE OP_DISABLE_HTTP)
target_link_libraries(opusfile PUBLIC opus ogg)

add_executable(frame-home
    src/main.cpp
    src/hsl_renderer.cpp
    src/hsl_renderer_splat.cpp
    src/panorama.cpp
    "${TP}/stb/stb_vorbis_impl.c"
    "${TP}/miniaudio/miniaudio_impl.c")
target_include_directories(frame-home PRIVATE "${TP}/openvr" "${TP}/miniaudio")
target_link_libraries(frame-home PRIVATE frame-scene opusfile ${FH_VULKAN} ${FH_OPENVR})
if(FRAME_HOME_PC)
    target_compile_definitions(frame-home PRIVATE FH_USE_VOLK)
endif()
if(WIN32)
    target_link_libraries(frame-home PRIVATE psapi ole32 shell32 winmm)
    # GUI subsystem: no console window ever opens, however SteamVR or the panel launches it (it logs to a file, not a
    # terminal — stderrIsTerminal() is already false on Windows). mingw keeps main() with -mwindows.
    target_link_options(frame-home PRIVATE -static -mwindows)
else()
    target_link_libraries(frame-home PRIVATE dl m)
endif()
if(NOT FRAME_HOME_PC)
    set_target_properties(frame-home PROPERTIES BUILD_RPATH "${OPENVR_LIB_DIR}" INSTALL_RPATH "${OPENVR_LIB_DIR}")
endif()

# fenv-tool: convert homes and models into .fenv environments.
add_executable(fenv-tool src/fenv_tool.cpp)
target_link_libraries(fenv-tool PRIVATE frame-scene)

# frame-switcher: the SteamVR dashboard panel that switches the environment.
add_executable(frame-switcher src/switcher.cpp)
target_include_directories(frame-switcher PRIVATE src "${TP}/stb" "${TP}/qhe" "${TP}/miniz" "${TP}/openvr")
# Its Settings page shows the version: major.minor only (with the installed files' hash as the build), so a commit
# that doesn't touch Frame Home leaves its binaries, and the hash updates go by, the same.
file(READ "${CMAKE_SOURCE_DIR}/VERSION" FRAME_VERSION_BASE)
string(STRIP "${FRAME_VERSION_BASE}" FRAME_VERSION_BASE)
target_compile_definitions(frame-switcher PRIVATE FRAME_VERSION_BASE="${FRAME_VERSION_BASE}")
if(FRAME_HOME_PC)
    target_include_directories(frame-switcher PRIVATE "${TP}/vulkan-headers/include")
endif()
target_link_libraries(frame-switcher PRIVATE frame-scene ${FH_OPENVR})
if(WIN32)
    target_link_libraries(frame-switcher PRIVATE psapi ole32 shell32)
    target_link_options(frame-switcher PRIVATE -static -mwindows)   # GUI subsystem: no console window ever opens
endif()
if(NOT FRAME_HOME_PC)
    set_target_properties(frame-switcher PROPERTIES BUILD_RPATH "${OPENVR_LIB_DIR}" INSTALL_RPATH "${OPENVR_LIB_DIR}")
endif()

# Tests (tests_loco/): -DFRAME_TESTS=ON, then build frame-tests and run ctest. Off by default: release builds skip them.
option(FRAME_TESTS "Build the tests in tests_loco (cmake --build <dir> --target frame-tests; ctest --test-dir <dir>)" OFF)
if(FRAME_TESTS)
    enable_testing()
    add_executable(loco_test tests_loco/loco_test.cpp)       # locomotion: floors, seats, stairs, gravity
    add_executable(clip_test tests_loco/clip_test.cpp)       # locomotion never puts eyes in walls or feet through floors
    foreach(t loco_test clip_test)
        target_include_directories(${t} PRIVATE "${TP}/openvr")
        target_link_libraries(${t} PRIVATE frame-scene ${FH_OPENVR})
        if(NOT FRAME_HOME_PC)
            set_target_properties(${t} PROPERTIES BUILD_RPATH "${OPENVR_LIB_DIR}")
        endif()
    endforeach()
    add_executable(spv_check_test tests_loco/spv_check_test.cpp)   # the shader checker, on every shader Frame Home ships
    target_include_directories(spv_check_test PRIVATE src)
    add_test(NAME loco COMMAND loco_test)
    add_test(NAME clip COMMAND clip_test)
    add_test(NAME spv_check COMMAND spv_check_test)
    set(FRAME_TEST_TARGETS loco_test clip_test spv_check_test)
    if(NOT WIN32)   # the Frame Home <-> compositor layer shared state (Linux: memfd, the layer itself)
        add_executable(env_share_test tests_loco/env_share_test.cpp)
        target_include_directories(env_share_test PRIVATE src "${TP}/vulkan-headers/include")
        target_link_libraries(env_share_test PRIVATE pthread)
        add_test(NAME env_share_thread COMMAND env_share_test 2 thread)
        add_test(NAME env_share_process COMMAND env_share_test 2 process)
        list(APPEND FRAME_TEST_TARGETS env_share_test)
    endif()
    add_custom_target(frame-tests DEPENDS ${FRAME_TEST_TARGETS})
endif()
