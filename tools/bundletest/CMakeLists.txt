# Environment bundles (src/fenv_bundle.h), checked natively on Linux (WSL) against real .fenv files:
#   bundletest <workdir> a.fenv b.fenv [c.fenv...] [--big]
#       bundles them (the first twice: the copy must come out as "<name> 2"), reads the bundle's and each
#       environment's manifest and thumbnail in place, validates, unpacks (byte for byte against the originals),
#       unpacks again (nothing new) and beside a different file of the same name; refuses what isn't an environment.
#       --big: also a bundle past 4 GB (zip64: a 2.3 GB made-up environment, twice), ~9 GB of disk for a minute
# cmake -S tools/bundletest -B <build> -G Ninja && ninja -C <build>
cmake_minimum_required(VERSION 3.16)
project(bundletest C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Release)
set(R ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(TP ${R}/third_party)
add_library(miniz STATIC ${TP}/miniz/miniz.c ${TP}/miniz/miniz_tdef.c ${TP}/miniz/miniz_tinfl.c ${TP}/miniz/miniz_zip.c)
target_include_directories(miniz PUBLIC ${TP}/miniz)
target_compile_definitions(miniz PRIVATE MINIZ_DISABLE_ZIP_READER_CRC32_CHECKS)   # as the real builds have it
add_library(lzma STATIC ${TP}/lzma/LzmaEnc.c ${TP}/lzma/LzmaDec.c ${TP}/lzma/LzFind.c ${TP}/lzma/LzFindOpt.c ${TP}/lzma/LzFindMt.c ${TP}/lzma/Threads.c ${TP}/lzma/CpuArch.c)
target_include_directories(lzma PUBLIC ${TP}/lzma)
add_executable(bundletest bundletest.cpp ${R}/src/fenv_lzma.cpp)
target_include_directories(bundletest PRIVATE ${R}/src ${TP}/qhe)
target_link_libraries(bundletest miniz lzma pthread)
