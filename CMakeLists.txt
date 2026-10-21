cmake_minimum_required(VERSION 3.16)
project(frame_home LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# SteamVR ships the OpenVR client library; link against the runtime's own copy.
set(OPENVR_LIB_DIR "/opt/steamvr/bin/linuxarm64" CACHE PATH "Directory containing libopenvr_api.so")
find_library(OPENVR_LIB NAMES openvr_api PATHS "${OPENVR_LIB_DIR}" NO_DEFAULT_PATH REQUIRED)
find_package(Vulkan REQUIRED)

set(TP "${CMAKE_SOURCE_DIR}/third_party")

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

# The home loader: HSL / Clay / glTF homes, .fenv environments (baked scenes), Blender / Unity models.
add_library(frame-scene STATIC
    src/hsl_scene.cpp
    src/scene_bake.cpp
    src/anim_bake.cpp
    src/fenv_lzma.cpp
    src/model_import.cpp
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
target_link_libraries(frame-scene PUBLIC miniz lzma smolv ${ASTCENC_LIB} pthread)
# libdeflate when the system has it (the Frame): APK scene.zips inflate several times faster than with miniz.
find_library(LIBDEFLATE_LIB deflate)
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
    src/panorama.cpp
    "${TP}/stb/stb_vorbis_impl.c"
    "${TP}/miniaudio/miniaudio_impl.c")
target_include_directories(frame-home PRIVATE "${TP}/openvr" "${TP}/miniaudio")
target_link_libraries(frame-home PRIVATE frame-scene opusfile Vulkan::Vulkan "${OPENVR_LIB}" dl m)
set_target_properties(frame-home PROPERTIES BUILD_RPATH "${OPENVR_LIB_DIR}" INSTALL_RPATH "${OPENVR_LIB_DIR}")

# fenv-tool: convert homes and models into .fenv environments.
add_executable(fenv-tool src/fenv_tool.cpp)
target_link_libraries(fenv-tool PRIVATE frame-scene)

# frame-switcher: the SteamVR dashboard panel that switches the environment.
add_executable(frame-switcher src/switcher.cpp)
target_include_directories(frame-switcher PRIVATE src "${TP}/stb" "${TP}/qhe" "${TP}/miniz" "${TP}/openvr")
target_link_libraries(frame-switcher PRIVATE frame-scene "${OPENVR_LIB}")
set_target_properties(frame-switcher PROPERTIES BUILD_RPATH "${OPENVR_LIB_DIR}" INSTALL_RPATH "${OPENVR_LIB_DIR}")
