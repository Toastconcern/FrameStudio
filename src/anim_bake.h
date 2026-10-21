#pragma once
// A Clay (.opa / .ovrscene) or glTF home's CPU animation as data, for the baked scene (scene.bin v13): everything the
// loader's animateRuntime(t) reads (the node hierarchy and its keyed tracks, skins and their clips, UV scroll /
// flipbook and tint tracks, vertex-animation offsets, each animated mesh's base vertices and its rigid-or-rewritten
// choice), and nothing the parse needed besides. A .fenv's animation is rebuilt from it as an OpaLoader / GltfLoader
// holding only that state, so it plays through the loaders' own animate() — the same code, the same numbers.
// Worked out at the bake: the rigid choices, each UV track's scroll-or-flipbook answer, and the nodes / tracks / clips
// no animated mesh depends on (left out).
#include "hsl_scene.h"

#include <memory>
#include <utility>
#include <vector>

class OpaLoader;
class GltfLoader;

namespace animbake {

// A parsed home's animation, given to its scene: scene.animate plays it through the loader's animateRuntime(t) and
// scene.saveAnimation writes it. A .fenv's rebuilt one plays through the same function (one compiled copy of the
// code, so no compiler choice — operand order, fused multiply-adds — can make the two differ).
void adopt(HslScene& scene, std::shared_ptr<OpaLoader> loader);
void adopt(HslScene& scene, std::shared_ptr<GltfLoader> loader);

// The loader's animation, written as data (read only: call it from the thread that animates the scene).
void save(const OpaLoader& loader, std::vector<u8>& out);
void save(const GltfLoader& loader, std::vector<u8>& out);

// The scene's animation from what save() wrote: scene.animate plays it, scene.saveAnimation writes the same bytes
// again. `meshes`: how many meshes the scene has (an animation that writes past them is damage). False: damaged.
bool load(const std::vector<u8>& bytes, size_t meshes, HslScene& scene);

// Every mesh the animation writes (animatedMeshes is the ones seen moving at a few times; this is all of them).
std::vector<int> targets(const std::vector<u8>& bytes);

// The meshes the animation writes as a fresh load has them (the animation at t = 0, from a new animator), copied, by
// mesh index; `bytes` becomes the animation as that animator saves it (its rigid choices made). False: damaged.
bool startPose(std::vector<u8>& bytes, const std::vector<MeshData>& meshes, std::vector<std::pair<size_t, MeshData>>& posed);

// The same in place: the scene's animated meshes put back where a load has them (a copy of a scene Frame Studio has
// been playing, before the optimizer reads its vertices). False: it has no stored animation, or it's damaged.
bool toStart(HslScene& scene);

}  // namespace animbake
