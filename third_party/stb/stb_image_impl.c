// stb_image implementation TU — provides JPEG decoding for glTF skybox panoramas
// (e.g. hogwarts custom homes ship a 20 MB pano.jpg; the cooked envs use ASTC/KTX, but these
// raw glTF .ovrscene homes reference image/jpeg, which our hand-rolled PNG/KTX path can't decode).
// JPEG-only keeps the build lean; PNG keeps its existing custom decoder in gltf_loader.h.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG      // Blender round-trip import: re-load the exported PNG textures (gltf_import.h, stbi_load_from_memory)
#define STBI_ONLY_TGA      // Unity / Blender model imports (model_import.cpp)
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF      // an environment's picture, model textures: the first frame
#define STBI_ONLY_PSD      // Photoshop files as pictures and textures: the flattened image
#define STBI_ONLY_HDR      // Radiance .hdr panoramas as an environment's sky (model_import.cpp)
#define STBI_NO_STDIO
// A tiny damaged file claiming a picture of millions of pixels a side made stb allocate gigabytes, or spend minutes
// filling one: nothing Frame reads is wider or taller than this (a 16K panorama is 16384 x 8192).
#define STBI_MAX_DIMENSIONS 16384
#include "stb_image.h"
