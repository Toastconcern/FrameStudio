#pragma once
// A loaded home, baked: everything HslScene holds after the slow part of loading (APK unzip, HSTF / RENDMESH /
// RENDSHAD parsing, SMOL-V decode, lightmap HDR decode, override resolution) written as one binary blob, so a
// .fenv opens in a fraction of the time. Textures (HDR lightmaps too) stay as their cooked ASTC chains (the Frame's GPU samples them
// directly), so the blob is GPU-ready and about the size of the home's own data.
//
// Clay (.opa / .ovrscene) and glTF homes animate on the CPU: their animation goes in as data too (anim_bake.h), played by
// the loader's own code from it.
#include "hsl_scene.h"

#include <string>
#include <vector>

constexpr u32 kSceneBakeVersion = 15;  // 2: world; 3: HDR maps as ASTC, binary world; 4: roles; 5: mesh refs; 6: gravity; 7: mesh paths; 8: ambience name; 9: compressed 8-bit lightmaps; 10: reflection cube as ASTC; 11: walkable area (and simplified colliders); 12: prepared and specialized shaders; 13: Clay / glTF CPU animation; 14: wall spots removed; 15: the environment has splats (nothing new in here: they're .fenv members, and a reader older than splats must refuse it rather than show it without them)
// What a scene is written as: the oldest version that holds it, so one without splats stays 14 (older Frame Homes still
// open it). A reader of a file needs at least this.
constexpr u32 kSceneBakeVersionNoSplats = 14;
inline u32 sceneBakeVersionFor(const HslScene& scene) { return scene.splats.empty() ? kSceneBakeVersionNoSplats : kSceneBakeVersion; }

struct VisibilityResult;
struct ObjectEdit;

// What a bake leaves out or adds beyond the scene itself: the visibility pass's index lists (visibility_bake.h) and
// the walkable area to carry (the scene's own when empty).
struct BakeOptions {
    const VisibilityResult* visibility = nullptr;
    std::string walkGrid;
};

// False (with `error`) when the scene can't be baked (an animation that can't be stored).
bool bakeScene(const HslScene& scene, std::vector<u8>& out, std::string& error, const BakeOptions* options = nullptr);

// The mesh optimizer for a save: the visibility pass over the scene's walkable area (Frame Studio's object edits kept
// whole), its result in `vis` and the options for bakeScene in `options` (the walkable area it was worked out from
// goes with it: Frame Home must not bake one from the thinned meshes). False when it didn't run (`vis.skipped`).
// FH_NO_VIS_CULL=1: never.
bool optimizeForBake(const HslScene& scene, const std::vector<ObjectEdit>* edits, VisibilityResult& vis, BakeOptions& options);
bool unbakeScene(const u8* data, size_t size, HslScene& scene, std::string& error);
