#pragma once
// Export to Blender: the home as glTF 2.0 binary (.glb), which Blender opens as it is (File > Import > glTF 2.0),
// in two files so the walkable area can be deleted or edited on its own:
//
//   <name>.glb          the environment: every object as a node holding its mesh (positions, normals, UV0, and the
//                       lightmap unwrap as UV1), grouped under a node per folder it sits in; one material per distinct
//                       look (base colour texture and tint, emissive texture, cut-out / blended, one- or two-sided);
//                       the sky as a node of its own named "Sky". Textures still compressed (ASTC) are decoded and
//                       embedded as PNG, each image once however many materials use it.
//   <name>_navmesh.glb  the walk layer only: "Navmesh" (the walkable area's triangles, as Studio's green overlay
//                       draws them, welded into one mesh), "Colliders" (the world's boxes, spheres, capsules and mesh
//                       colliders as meshes), "Spawns", "Seats" and "Hotspots" (empties placed and turned as in the
//                       world). Plain translucent materials, no textures.
//
// Frame's space is Y-up and in metres, as glTF's is: nothing is converted (Blender's importer turns +Y up into +Z up).
//
// What a material carries (glTF has no lightmap slot):
//   baseColorTexture   the texture the home's shader samples as its base colour (the sampler named BaseColor /
//                      albedo / diffuse, else the loader's base map), sRGB
//   baseColorFactor    MeshData::tint (clamped to 0..1)
//   emissiveTexture    the material's emissive map, when it has one
//   occlusionTexture   the baked lightmap, on UV1 (its atlas scale / offset applied to UV1), tonemapped to 8 bits:
//                      linear radiance clamped to 0..1 and sRGB-encoded. Blender keeps it in the material's glTF
//                      output group (it doesn't darken the render): there to be wired in by hand
//   alphaMode          BLEND for blended (and additive) meshes, MASK with alphaCutoff for cut-outs, else OPAQUE
//   doubleSided        unless the mesh culls its back faces (MeshData::cullBack)
// Not exported: normal / ORM maps (Meta's packed ONxRNy / RBAoDir aren't glTF's), vertex colours (wind masks, not
// colour), animation, splats, gravity zones, UV scrolls and shader tiling factors.
#include "hsl_scene.h"

#include <functional>
#include <string>
#include <vector>

struct WalkGrid;

namespace blender {

// One object to export: a scene mesh drawn with a world matrix (Studio: Object::pivot * Object::offset).
struct ExportObject {
    int mesh = -1;                      // HslScene::meshes index
    float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // column-major, as drawn
    std::string name;                   // its node's name in Blender
    std::vector<std::string> folders;   // where it sits (MeshData::path split), outermost first: a node each
};

struct ExportOptions {
    unsigned threads = 0;               // texture decode / PNG threads (0: the cores, at most 8)
    unsigned maxTextureSide = 4096;     // textures bigger than this go in at the first level within it
    bool lightmaps = true;              // baked lightmaps as the materials' occlusion textures (on UV1)
    // Called now and then from the exporting thread: done 0..1, and what it's on.
    std::function<void(float, const char*)> progress;
};

struct ExportStats {
    // the environment
    size_t nodes = 0, meshes = 0, materials = 0, images = 0, triangles = 0, skipped = 0;   // skipped: no triangles / bad index
    size_t imagesFailed = 0;            // textures that couldn't be decoded (their materials go without)
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};   // world bounds of every exported vertex
    // the walk layer
    size_t navTriangles = 0, navVertices = 0, colliders = 0, points = 0;
};

// The environment file. Writes `path` (a .part file beside it while it's being written).
bool exportScene(const HslScene& scene, const std::vector<ExportObject>& objects, const std::string& path, std::string& error,
                 const ExportOptions& options = {}, ExportStats* stats = nullptr);

// The walk layer file: `walk`'s walkable area (none: no Navmesh node) and `world`'s colliders, spawns, seats and
// hotspots.
bool exportNavmesh(const WalkGrid* walk, const HomeWorld& world, const std::string& path, std::string& error, ExportStats* stats = nullptr);

// "<dir>/<name>_navmesh.glb" for "<dir>/<name>.glb".
std::string navmeshPathFor(const std::string& glbPath);

}  // namespace blender

// Both files: the environment to glbPath, the walk layer to navmeshPath ("" leaves it out).
bool exportBlender(const HslScene& scene, const std::vector<blender::ExportObject>& objects, const WalkGrid* walk, const HomeWorld& world,
                   const std::string& glbPath, const std::string& navmeshPath, std::string& error,
                   const blender::ExportOptions& options = {}, blender::ExportStats* stats = nullptr);
