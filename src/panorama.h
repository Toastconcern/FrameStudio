#pragma once
// Homes as the Steam Frame's native environment.
//
// SteamVR's compositor draws its environment itself from an equirectangular background image
// (8192x4096, like its own night_mountains.png), optionally dome-projected: the image is projected
// from the viewer's eye height onto a sphere of a given radius with a flat floor, so the floor and the
// room's walls appear at roughly the right distance. No app runs; the compositor owns it.
//
// A home is rendered with its own Meta shaders into six 90-degree faces around the viewer and
// resampled into that layout. The image centre is the direction `yawDeg` faces (0 = -Z, the home's
// forward), with the image's right-hand side to the viewer's right.
#include "hsl_renderer.h"
#include "hsl_scene.h"

#include <string>
#include <vector>

struct PanoramaView {
    float eye[3] = {0, 1.6f, 0};        // where the viewer stands (eye height above the home's floor)
    float yawDeg = 0;                   // direction that becomes the image centre
};

// Renders width x width/2 RGB8 (sRGB). Leaves the renderer resized to the face size.
bool renderPanorama(HslRenderer& r, const PanoramaView& view, uint32_t width, std::vector<u8>& rgb);

// Median horizontal distance from `eye` to the home's visible geometry: the dome radius that puts the
// projected walls where they are. Falls back to `fallback` when nothing is hit (open outdoor homes).
float panoramaDomeRadius(const HslRenderer& r, const HslScene& scene, const float eye[3], float fallback = 10.0f);

bool writePng(const std::string& path, const std::vector<u8>& rgb, uint32_t w, uint32_t h);
