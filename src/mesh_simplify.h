#pragma once
// Fewer triangles, the same picture: where a mesh's surface is flat and everything a shader reads across it changes
// evenly (texture and lightmap coordinates, colours, normals, tangents), the triangles there are rebuilt as fewer,
// bigger ones (meshoptimizer's simplifier, third_party/meshoptimizer). Nothing moves by more than kTolerance and no
// attribute drifts by more than a hair, so it draws the same. Only an index list changes: the vertices stay as they
// are (a shader may number them), the new triangles use some of them.
#include "hsl_scene.h"
#include "meshoptimizer.h"

#include <algorithm>
#include <vector>

namespace meshsimplify {

constexpr float kTolerance = 1e-4f;      // m off the surface
constexpr float kAttributeWeight = 100;  // an attribute drifting by kTolerance / 100 already costs the whole budget

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
        for (const auto& ch : channels) if (ch.data->size() >= vertices_ * ch.width) width_ += ch.width;
        if (colors) width_ += 4;
        attributes_.resize(vertices_ * width_);
        for (size_t v = 0; v < vertices_; ++v) {
            float* a = &attributes_[v * width_];
            for (const auto& ch : channels)
                if (ch.data->size() >= vertices_ * ch.width) a = std::copy_n(&(*ch.data)[v * ch.width], ch.width, a);
            if (colors) for (int k = 0; k < 4; ++k) *a++ = md.colors[v * 4 + k] / 255.0f;
        }
        weights_.assign(width_, kAttributeWeight);
    }

    // The triangles `indices` (of this mesh) as few as they can be with nothing changing. Their open edges stay where
    // they are (another part of the mesh, simplified on its own, meets them there). Unchanged when nothing merges.
    std::vector<u32> simplify(const std::vector<u32>& indices) const {
        if (indices.size() < 6 || width_ > 32 || !vertices_) return indices;
        for (u32 i : indices) if (i >= vertices_) return indices;
        std::vector<u32> out(indices.size());
        constexpr unsigned kOptions = meshopt_SimplifyLockBorder | meshopt_SimplifyErrorAbsolute;
        const size_t n = !width_ ? meshopt_simplify(out.data(), indices.data(), indices.size(), positions_.data(), vertices_, 3 * sizeof(float), 0, kTolerance, kOptions, nullptr)
                                 : meshopt_simplifyWithAttributes(out.data(), indices.data(), indices.size(), positions_.data(), vertices_, 3 * sizeof(float),
                                                        attributes_.data(), width_ * sizeof(float), weights_.data(), width_, nullptr, 0, kTolerance, kOptions, nullptr);
        if (!n || n >= indices.size()) return indices;
        out.resize(n);
        return out;
    }

private:
    size_t vertices_ = 0, width_ = 0;
    std::vector<float> positions_, attributes_, weights_;
};

}  // namespace meshsimplify
