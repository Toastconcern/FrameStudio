#pragma once
// A Meta HSL home (v203–v210) loaded for rendering. On Quest a home is a FOOTPRINT (the room, e.g.
// Haven2025) plus a VISTA (the world outside, e.g. Calming) loaded together; the vista also ships the
// baked lightmaps for the footprint's meshes. Both load here as one scene: the editor's SceneLoader
// meshes from each APK, every shader program they ship (forward pass, SMOL-V decoded + names restored),
// the baked lightmaps resolved across the pair, and the real ambient light.
#include "core/types.h"
#include "core/world.h"
#include "walkgrid.h"
#include "loaders/rendskel_parser.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

struct ShaderProgramSrc {
    std::string path;                   // scene.zip asset path without "content/" (matches MeshData::shaderPath)
    std::vector<u32> vert, frag;
    // Made by the bake (shader_bake.h; a .fenv carries them), else by the renderer at load. prepared: the two below are
    // made (fragAlpha empty: the shader has no alpha-only form).
    bool prepared = false;
    std::vector<u32> vertInvariant, fragAlpha;
    // Per distinct set of material values: the fragment shader with them baked in, by shaderbake::variantKey (frag
    // empty: nothing bakes in, the shader as shipped).
    struct Variant { uint64_t key = 0; std::vector<u32> frag, fragAlpha; };
    std::vector<Variant> variants;
};

// A skeleton + its clip driving skinned meshes (birds, butterflies, creatures). Bone indices of those meshes
// are already skeleton joint indices, so the sampled per-joint skin matrices go straight to the GPU.
struct HslAnimGroup {
    RendSkel skel;
    RendClip clip;
    std::vector<int> meshes;            // HslScene::meshes indices
};

// What one APK of the home is (the editor's info panel).
struct HslSourceInfo {
    std::string package, version;       // from AndroidManifest.xml: com.meta.shell.env.footprint.haven2025, 206.0.0.0.70
    std::string format;                 // "HSL", "Clay (.opa)" or "Clay (.ovrscene)" (SideQuest-style custom homes: glTF in scene.zip)
    std::string role;                   // "Footprint" (the room), "Vista" (the world outside) or "Home" (both in one)
};

struct HslScene {
    std::vector<std::string> apkPaths;  // footprint first, then the vista (if any)
    std::vector<MeshData> meshes;       // all APKs' meshes, in apkPaths order
    std::vector<int> meshSource;        // per mesh: index into apkPaths
    std::vector<HslAnimGroup> anims;
    std::map<std::string, ShaderProgramSrc> programs;   // by asset path
    float ambientRGB[3] = {0.95f, 0.90f, 0.83f};        // radiance; replaced by the env's .lprb L00 when present
    bool hasEnvAmbient = false;
    // Clay / glTF homes: evaluates the home's authored animation at t seconds (node motion, skinning, UV flipbooks
    // and scrolls such as the fire, tint fades, vertex-animation textures) by rewriting the affected meshes'
    // positions / uvs / curTint on the CPU. Empty for HSL homes: their rigs and shaders animate from the frame time.
    std::function<void(float, std::vector<MeshData>&)> animate;
    // The same animation as data (anim_bake.h), for a bake: what a .fenv plays it from. Set with animate; it reads the
    // animator, so call it from the thread that animates the scene.
    std::function<void(std::vector<u8>&)> saveAnimation;
    std::vector<int> animatedMeshes;    // the meshes animate() changes (their vertices re-upload every frame)
    float animDuration = 0;             // the authored loop length in seconds (0 = none authored)
    std::vector<u8> ambientOgg;         // the home's looping ambience (_BACKGROUND_LOOP.ogg), if it ships one
    std::string ambientSource;          // where it came from: "Ogg loop", "FMOD bank" or empty (none)
    std::string ambientName;            // its own name in the home: the Ogg file's, or the bank sound's ("amb_CalmingVista_...")
    std::vector<HslSourceInfo> sources; // per apkPaths entry
    std::vector<std::vector<std::string>> sourceFiles;   // per apkPaths entry: the files an imported model read
    std::string fenvPath, fenvEdits;    // opened from a .fenv: the file, and its edits.json (extracted; "" if none)
    HomeWorld world;                    // spawns, seats, colliders, walkable areas (Meta's, or a .fenv's world.json)
};

// Loads one or more APKs as a single home (footprint + vista). Returns false (with `error`) when an APK
// is not an HSL home or fails to parse.
bool loadHslScene(const std::vector<std::string>& apkPaths, HslScene& scene, std::string& error);
inline bool loadHslScene(const std::string& apkPath, HslScene& scene, std::string& error) {
    return loadHslScene(std::vector<std::string>{apkPath}, scene, error);
}

// The world to walk in: the home's own (Meta's converted, or a .fenv's) when it has floors or walls; else floors and
// walls taken from the drawn geometry (opaque, not the sky, not distant scenery) — Clay homes and homes like Theater
// ship no collision at all. Spawns, seats and wall spots are kept either way.
// The world carries its walkable area (walkGridCache): the one Frame Studio baked (world.json), else one baked here from
// the drawn geometry plus the colliders (bakeWalkGrid). Gravity homes also get the geometry's floors and walls as
// colliders while nothing blocks (the gravity walker reads colliders).
HomeWorld walkWorld(const HslScene& scene);
std::vector<WorldCollider> geometryColliders(const HslScene& scene);   // floors + walls from the drawn meshes

// Bakes the home's walkable area from everything drawn that's solid (opaque, not the sky or the distant backdrop;
// see-through solids block but aren't floor) and the world's colliders (roles override: a mesh a collider is
// made from — "mesh" names it — is left to that collider). meshWorld: per mesh, its world matrix when it was moved
// (Frame Studio; nullptr: as loaded); removed: per mesh, left out (deleted). Report tags: mesh index, or
// meshes.size() + collider index.
std::shared_ptr<const WalkGrid> bakeWalkGrid(const HslScene& scene, const HomeWorld& world, const WalkParams& params, WalkReport* report = nullptr,
                                             const std::vector<const float*>* meshWorld = nullptr, const std::vector<char>* removed = nullptr);
// The home's drawn solid meshes as walk-bake sources, in world space (meshWorld / removed: Frame Studio's edits).
// leaveLinked: meshes a Studio surface stands for are left out (the bake; the world generators want them all).
std::vector<WalkSource> drawnWalkSources(const HslScene& scene, const HomeWorld& world, const std::vector<const float*>* meshWorld = nullptr,
                                         const std::vector<char>* removed = nullptr, bool leaveLinked = true);
// Is this mesh in the walkable-area bake: 0 no, 1 solid, 2 blocks but isn't floor (see-through).
int walkMeshUse(const MeshData& md);

// Finds the program a mesh's material references (exact asset path, else same ".surface" name).
const ShaderProgramSrc* findProgram(const HslScene& scene, const std::string& shaderPath);
