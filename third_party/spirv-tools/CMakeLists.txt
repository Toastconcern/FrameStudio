# SPIRV-Tools (Apache-2.0, LICENSE here), its optimizer and the validator it runs: commit ef96ed763b43 (the one
# glslang 16.6.0 pins), with SPIRV-Headers 496543121ce6 beside it (third_party/spirv-headers). Only what the optimizer
# needs is kept: include/, source/*.cpp, source/util, source/val, source/opt.
# Upstream's build makes its grammar tables with Python; generated/ holds them made once from the same two commits
# (utils/ggt.py, generate_language_headers.py for DebugInfo and OpenCLDebugInfo100, generate_registry_tables.py,
# update_build_version.py), so no build here needs Python.
# (none of the including project's warning flags: MSVC warns when /w overrides its /W3)
set_property(DIRECTORY PROPERTY COMPILE_OPTIONS "")
file(GLOB SPVTOOLS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/util/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/val/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/source/opt/*.cpp")
add_library(spirv-tools-opt STATIC ${SPVTOOLS_SOURCES})
target_include_directories(spirv-tools-opt
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/generated" "${CMAKE_CURRENT_SOURCE_DIR}/../spirv-headers/include")
target_compile_features(spirv-tools-opt PUBLIC cxx_std_17)
if(WIN32)
    target_compile_definitions(spirv-tools-opt PRIVATE SPIRV_WINDOWS _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
    target_compile_options(spirv-tools-opt PRIVATE /utf-8 /bigobj /w)
else()
    target_compile_options(spirv-tools-opt PRIVATE -w)
endif()
