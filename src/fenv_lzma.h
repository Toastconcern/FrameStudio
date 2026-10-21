#pragma once
// LZMA for a .fenv's scene data: the strongest lossless coder we found for ASTC textures (about a fifth smaller than
// deflate on real homes). Tuned for ASTC: position bits on the 16-byte block (lc0 lp4 pb4), optimal parsing.
//   Smallest: one stream with a dictionary as large as the data (up to 256 MB); the match finder runs on its own
//             thread. Every repeat anywhere in the scene is found.
//   Fast:     up to 8 chunks coded at once (~3% bigger, several times quicker to save).
// Either way the chunks unpack in parallel.
// Stream "FLZ2": u32 chunk count, then per chunk: 5 property bytes, u64 raw size, u64 packed size, LZMA data.
// ("FLZ1" — one chunk, no count — from earlier builds still unpacks.)
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

enum class FenvPacking { Smallest, Fast };

// progress(done, total) is called from the coding threads as input is consumed (may be empty).
bool fenvLzmaCompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error,
                      FenvPacking packing = FenvPacking::Smallest, const std::function<void(size_t, size_t)>& progress = {});
bool fenvLzmaIsStream(const uint8_t* data, size_t size);
bool fenvLzmaDecompress(const uint8_t* data, size_t size, std::vector<uint8_t>& out, std::string& error);
