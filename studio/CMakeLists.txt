# Frame Studio — desktop app for Windows, Linux and macOS (same renderer as Frame Home, in a window with an editor
# UI). Everything it needs is vendored: Vulkan is loaded at runtime through volk (MoltenVK on macOS); the window is
# Win32 on Windows and GLFW elsewhere (studio/platform.h has the rest of the system). Presets: CMakePresets.json.
cmake_minimum_required(VERSION 3.16)
project(frame_studio LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(TP "${ROOT}/third_party")

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS VK_USE_PLATFORM_WIN32_KHR)
endif()
if(MSVC)
    add_compile_options(/utf-8 /bigobj /W3 /wd4244 /wd4267 /wd4305)
endif()

add_library(volk STATIC "${TP}/volk/volk.c")
target_include_directories(volk PUBLIC "${TP}/volk" "${TP}/vulkan-headers/include")

# LZMA SDK (public domain): the .fenv scene coder (src/fenv_lzma.cpp), with its threaded match finder.
add_library(lzma STATIC "${TP}/lzma/LzmaEnc.c" "${TP}/lzma/LzmaDec.c" "${TP}/lzma/LzFind.c" "${TP}/lzma/LzFindOpt.c"
    "${TP}/lzma/LzFindMt.c" "${TP}/lzma/Threads.c" "${TP}/lzma/CpuArch.c")
target_include_directories(lzma PUBLIC "${TP}/lzma")
add_library(miniz STATIC "${TP}/miniz/miniz.c" "${TP}/miniz/miniz_tdef.c" "${TP}/miniz/miniz_tinfl.c" "${TP}/miniz/miniz_zip.c")
target_include_directories(miniz PUBLIC "${TP}/miniz")
# APKs are signed and intact; re-checksumming every extracted asset was ~20% of load time.
target_compile_definitions(miniz PRIVATE MINIZ_DISABLE_ZIP_READER_CRC32_CHECKS)
add_library(smolv STATIC "${TP}/smolv/smolv.cpp")
target_include_directories(smolv PUBLIC "${TP}/smolv")

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ASTCENC_CLI OFF CACHE BOOL "" FORCE)
set(ASTCENC_DECOMPRESSOR OFF CACHE BOOL "" FORCE)
set(ASTCENC_UNIVERSAL_BUILD OFF CACHE BOOL "" FORCE)
# astcenc's SIMD: NEON on ARM (Windows on ARM, Apple Silicon, ARM Linux), SSE4.1 on x86-64.
# (on macOS the architecture asked for decides: an Apple Silicon Mac builds the Intel version too)
if(CMAKE_OSX_ARCHITECTURES)
    set(STUDIO_ARCH "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(STUDIO_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if(STUDIO_ARCH MATCHES "^(arm64|ARM64|aarch64|AARCH64)$")
    set(ASTCENC_ISA_NEON ON CACHE BOOL "" FORCE)
    set(STUDIO_ASTCENC astcenc-neon-static)
else()
    set(ASTCENC_ISA_SSE41 ON CACHE BOOL "" FORCE)
    set(STUDIO_ASTCENC astcenc-sse4.1-static)
endif()
add_subdirectory("${TP}/astcenc" astcenc EXCLUDE_FROM_ALL)

# Ogg Opus ambience (Clay homes' _BACKGROUND_LOOP.ogg): Xiph libopus + libogg + opusfile (BSD), decode only.
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

# The window: Win32 on Windows; GLFW (X11 on Linux, Cocoa on macOS) elsewhere.
if(WIN32)
    set(STUDIO_IMGUI_BACKEND "${TP}/imgui/backends/imgui_impl_win32.cpp")
    set(STUDIO_PLATFORM platform_win32.cpp)
else()
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    # (Wayland sessions run it through XWayland; GLFW's own Wayland backend needs wayland-scanner to build)
    set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
    add_subdirectory("${TP}/glfw" glfw EXCLUDE_FROM_ALL)
    set(STUDIO_IMGUI_BACKEND "${TP}/imgui/backends/imgui_impl_glfw.cpp")
    set(STUDIO_PLATFORM platform_posix.cpp "${TP}/tinyfiledialogs/tinyfiledialogs.c")
endif()

add_library(imgui STATIC
    "${TP}/imgui/imgui.cpp" "${TP}/imgui/imgui_draw.cpp" "${TP}/imgui/imgui_tables.cpp" "${TP}/imgui/imgui_widgets.cpp"
    ${STUDIO_IMGUI_BACKEND} "${TP}/imgui/backends/imgui_impl_vulkan.cpp"
    "${TP}/imguizmo/ImGuizmo.cpp")
target_include_directories(imgui PUBLIC "${TP}/imgui" "${TP}/imguizmo")
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK GLFW_INCLUDE_NONE)
target_link_libraries(imgui PUBLIC volk)
if(NOT WIN32)
    target_link_libraries(imgui PUBLIC glfw)
endif()

add_executable(frame-studio WIN32 MACOSX_BUNDLE
    studio.cpp
    ${STUDIO_PLATFORM}
    "${ROOT}/src/hsl_scene.cpp"
    "${ROOT}/src/scene_bake.cpp"
    "${ROOT}/src/anim_bake.cpp"
    "${ROOT}/src/fenv_lzma.cpp"
    "${ROOT}/src/model_import.cpp"
    "${TP}/ufbx/ufbx.c"
    "${ROOT}/src/hsl_renderer.cpp"
    "${TP}/qhe/cook/hzanim_acl.cpp"
    "${TP}/stb/stb_image_impl.c"
    "${TP}/stb/stb_vorbis_impl.c"
    "${TP}/miniaudio/miniaudio_impl.c")
set_target_properties(frame-studio PROPERTIES OUTPUT_NAME "Frame Studio")
# ACL's NEON code mixes vector types GCC keeps apart by default (as the Frame's own build: CMakeLists.txt)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64|AARCH64)$")
    set_source_files_properties("${TP}/qhe/cook/hzanim_acl.cpp" PROPERTIES COMPILE_OPTIONS "-flax-vector-conversions")
endif()
target_compile_definitions(frame-studio PRIVATE FH_USE_VOLK)
target_include_directories(frame-studio PRIVATE
    "${ROOT}/src"
    "${TP}/cgltf"
    "${TP}/ufbx"
    "${TP}/qhe"
    "${TP}/stb"
    "${TP}/miniaudio"
    "${TP}/acl/includes"
    "${TP}/rtm/includes"
    "${TP}/astcenc/Source"
    "${TP}/tinyfiledialogs")
target_link_libraries(frame-studio PRIVATE imgui volk miniz lzma smolv opusfile ${STUDIO_ASTCENC})
if(WIN32)
    target_link_libraries(frame-studio PRIVATE dwmapi comdlg32 shell32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(frame-studio PRIVATE glfw Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
if(APPLE)
    set_target_properties(frame-studio PROPERTIES MACOSX_BUNDLE_BUNDLE_NAME "Frame Studio" MACOSX_BUNDLE_GUI_IDENTIFIER "com.framestudio.app")
endif()
# Frame Home's headset side (arm64 binaries + install.sh) ships beside the exe, in frame\: Frame > Install on Steam Frame.
add_custom_command(TARGET frame-studio POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${ROOT}/frame/payload" "$<TARGET_FILE_DIR:frame-studio>/frame")
if(MINGW)   # MinGW builds (x64, x86 and ARM64): wWinMain entry point, no runtime DLLs to ship
    target_link_options(frame-studio PRIVATE -municode -static)
endif()
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)   # 32-bit: up to 4 GB of address space on 64-bit Windows instead of 2 (big homes)
    if(MSVC)
        target_link_options(frame-studio PRIVATE /LARGEADDRESSAWARE)
    else()
        target_link_options(frame-studio PRIVATE "LINKER:--large-address-aware")
    endif()
endif()
