#pragma once
// Fewer triangles, the same picture: where a mesh's surface is flat and everything a shader reads across it changes
// evenly (texture and lightmap coordinates, colours, normals, tangents), the triangles there are rebuilt as fewer,
// bigger ones (meshoptimizer's simplifier, third_party/meshoptimizer). Nothing moves by more than the tolerance asked
// for (kTolerance up close; for what's only ever seen from farther, a quarter of a headset pixel there: toleranceAt)
// and no attribute drifts by more than an amount nobody sees (kDrift), so it draws the same. Only an index list changes:
// the vertices stay as they are (a shader may number them), the new triangles use some of them.
#include "hsl_scene.h"
#include "meshoptimizer.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace meshsimplify {

constexpr float kTolerance = 1e-4f;      // m off the surface, up close
constexpr float kAttributeWeight = 100;  // up close, an attribute drifting by kTolerance / 100 already costs the whole budget
// A headset pixel's angle (a Steam Frame eye: about 2160 pixels over 110 degrees), and the share of one a surface may move.
constexpr float kPixelAngle = 0.00089f, kPixelShare = 0.25f;
// The most each kind of attribute may drift, whatever the tolerance: a texture coordinate 1/12 of a texel of a 4k
// texture, a normal or a tangent a quarter of a degree (a fraction of a percent of its light), a colour half a step.
struct Drift { float uv = 2e-5f, normal = 0.004f, color = 0.5f / 255; };
constexpr Drift kDrift;
// How far something may move when the nearest any eye comes to it is `distance` (m): kTolerance, or a quarter of a
// headset pixel at that distance when that's more.
inline float toleranceAt(float distance) { return std::max(kTolerance, kPixelShare * kPixelAngle * distance); }

// One mesh, ready to simplify parts of its index list: its vertices in the world (the error is in metres whatever
// the mesh's own units) and, side by side, the attributes the renderer can hand a shader.
class Mesh {
public:
    // world: the mesh's world matrix (column-major); normals: per vertex, as it's shaded (visbake::pinnedNormals).
    Mesh(const MeshData& md, const float* world, const std::vector<float>& normals) {
        vertices_ = std::min<size_t>(md.nVerts, md.positions.size() / 3);
        positions_.resize(vertices_ * 3);
        for (size_t v = 0; v < vertices_; ++v) {
            const float* p = &md.positions[v * 3];
            for (int j = 0; j < 3; ++j) positions_[v * 3 + j] = world[j] * p[0] + world[4 + j] * p[1] + world[8 + j] * p[2] + world[12 + j];
        }
        // the channels every vertex has (one missing, or short: nothing to keep alike there)
        const bool colors = md.colors.size() >= vertices_ * 4;
        const struct { const std::vector<float>* data; size_t width; } channels[] = {
            {&md.uvs, 2}, {&md.uvs2, 2}, {&md.uvs3, 2}, {&md.uvs4, 2}, {&normals, 3}, {&md.tangents, 4}};
        // each channel's most drift (what a weight is worked out from per tolerance)
        const float drift[] = {kDrift.uv, kDrift.uv, kDrift.uv, kDrift.uv, kDrift.normal, kDrift.normal};
        for (size_t c = 0; c < std::size(channels); ++c)
            if (channels[c].data->size() >= vertices_ * channels[c].width) { width_ += channels[c].width; drift_.insert(drift_.end(), channels[c].width, drift[c]); }
        if (colors) { width_ += 4; drift_.insert(drift_.end(), 4, kDrift.color); }
        attributes_.resize(vertices_ * width_);
        for (size_t v = 0; v < vertices_; ++v) {
            float* a = &attributes_[v * width_];
            for (const auto& ch : channels)
                if (ch.data->size() >= vertices_ * ch.width) a = std::copy_n(&(*ch.data)[v * ch.width], ch.width, a);
            if (colors) for (int k = 0; k < 4; ++k) *a++ = md.colors[v * 4 + k] / 255.0f;
        }
    }

    // The triangles `indices` (of this mesh) as few as they can be with nothing changing, nothing moving more than
    // `tolerance` (m). Their open edges stay where they are (another part of the mesh, simplified on its own, meets them
    // there). Unchanged when nothing merges.
    std::vector<u32> simplify(const std::vector<u32>& indices, float tolerance = kTolerance) const {
        if (indices.size() < 6 || width_ > 32 || !vertices_) return indices;
        for (u32 i : indices) if (i >= vertices_) return indices;
        std::vector<u32> out(indices.size());
        constexpr unsigned kOptions = meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute;
        // an attribute's weight: drifting by its most (or by tolerance / kAttributeWeight, when that's less) costs the
        // whole budget
        std::vector<float> weights(width_);
        for (size_t k = 0; k < width_; ++k) weights[k] = std::max(kAttributeWeight, tolerance / drift_[k]);
        const size_t n = !width_ ? meshopt_simplify(out.data(), indices.data(), indices.size(), positions_.data(), vertices_, 3 * sizeof(float), 0, tolerance, kOptions, nullptr)
                                 : meshopt_simplifyWithAttributes(out.data(), indices.data(), indices.size(), positions_.data(), vertices_, 3 * sizeof(float),
                                                        attributes_.data(), width_ * sizeof(float), weights.data(), width_, nullptr, 0, tolerance, kOptions, nullptr);
        if (!n || n >= indices.size()) return indices;
        out.resize(n);
        return out;
    }

private:
    size_t vertices_ = 0, width_ = 0;
    std::vector<float> positions_, attributes_, drift_;   // drift_: per attribute float, the most it may drift
};

}  // namespace meshsimplify
