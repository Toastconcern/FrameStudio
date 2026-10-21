// stb_image implementation TU — provides JPEG decoding for glTF skybox panoramas
// (e.g. hogwarts custom homes ship a 20 MB pano.jpg; the cooked envs use ASTC/KTX, but these
// raw glTF .ovrscene homes reference image/jpeg, which our hand-rolled PNG/KTX path can't decode).
// JPEG-only keeps the build lean; PNG keeps its existing custom decoder in gltf_loader.h.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG      // Blender round-trip import: re-load the exported PNG textures (gltf_import.h, stbi_load_from_memory)
#define STBI_ONLY_TGA      // Unity / Blender model imports (model_import.cpp)
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#include "stb_image.h"
