#pragma once
// Models from Blender or Unity placed into a home: glTF 2.0 (.gltf / .glb — Blender's native export, Unity's
// glTFast / UnityGLTF), FBX and OBJ (both tools' classic exports). Each mesh part becomes one MeshData drawn
// with the built-in unlit textured program (the same one Clay homes use): base-colour texture x colour factor,
// alpha blending or cut-out from the material. Units become metres and up becomes +Y.
#include "core/types.h"

#include <string>
#include <vector>

bool isModelPath(const std::string& path);

// `files` receives every file the model read (itself, external buffers, textures), so a .fenv can carry them.
bool importModel(const std::string& path, std::vector<MeshData>& meshes, std::vector<std::string>& files, std::string& error);
