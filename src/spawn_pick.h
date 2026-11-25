// Where someone is put when a home opens, when it has no spawn of its own (Frame Studio's Auto spawn, Frame Home): on
// the walkable area, on level open floor with room round it (not against a wall, on a stair or in a doorway), facing
// the widest view from eye height. Meta's homes are made to be seen from their origin: when that's somewhere to stand,
// that's where.
#pragma once
#include "walkgrid.h"
#include <algorithm>
#include <cmath>

namespace spawnpick {

constexpr float kEyeHeight = 1.6f;      // standing eyes over the floor (m)
constexpr float kWantClear = 1.0f;      // room round the spot it aims for, to the area's edge (m)
constexpr float kFarthest = 25.0f;      // a view this long (or out past the home) is as open as a view gets (m)

// How far you can see from eyes at (x, eyeY, z) along (dx, dz) before something at about eye height is in the way.
inline float sightline(const WalkGrid& g, float x, float eyeY, float z, float dx, float dz) {
    for (float d = 0.3f; d < kFarthest; d += g.p.cell) {
        int cx, cz;
        if (!g.cellOf(x + dx * d, z + dz * d, cx, cz)) return kFarthest;   // out past the home: open
        for (const WalkSpan* s = g.begin(cx, cz); s != g.end(cx, cz); ++s)
            if (s->lo <= eyeY + 0.1f && s->hi >= eyeY - 0.6f) return d;
    }
    return kFarthest;
}

// The way to face from a spot on the floor (floorY): the widest view, a little smoothed so a slit between two walls
// doesn't win over an open side. Yaw as WorldPoint's (0 faces -Z).
inline float bestFacing(const WalkGrid& g, float x, float floorY, float z) {
    constexpr int kDirs = 48;
    float len[kDirs];
    for (int i = 0; i < kDirs; ++i) {
        const float a = i * 6.2831853f / kDirs;
        len[i] = sightline(g, x, floorY + kEyeHeight, z, std::sin(a), std::cos(a));
    }
    int best = 0;
    float bestScore = -1;
    for (int i = 0; i < kDirs; ++i) {
        float score = 0;
        for (int k = -3; k <= 3; ++k) score += len[(i + k + kDirs) % kDirs] * (4 - std::abs(k));
        if (score > bestScore) { bestScore = score; best = i; }
    }
    const float a = best * 6.2831853f / kDirs;
    return std::atan2(-std::sin(a), -std::cos(a));
}

// Level floor round a cell: its 8 neighbours on the area at the same height (within 3 cm: not a stair or a slope).
inline bool levelAround(const WalkGrid& g, int x, int z, const WalkSpan& s) {
    for (int k = 0; k < 9; ++k) {
        const WalkSpan* q = g.walkNear(x + k % 3 - 1, z + k / 3 - 1, s.hi, 0.03f, kWalkOk);
        if (!q) return false;
    }
    return true;
}

// A good spawn on the area: pos on the floor, yaw facing its best view. False: the area has nowhere to stand.
inline bool pick(const WalkGrid& g, float pos[3], float& yaw) {
    // the area: how much room its roomiest spot has, and its middle
    int most = 0;
    double sx = 0, sy = 0, sz = 0, n = 0;
    for (int z = 0; z < g.nz; ++z)
        for (int x = 0; x < g.nx; ++x)
            for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s)
                if (s->flags & kWalkOk) { most = std::max<int>(most, s->dist); sx += g.centerX(x); sy += s->hi; sz += g.centerZ(z); n += 1; }
    if (n == 0) return false;
    // aim for the origin when it's on the area (Meta's homes are made round it), else the area's middle
    float aim[3] = {static_cast<float>(sx / n), static_cast<float>(sy / n), static_cast<float>(sz / n)};
    float originFloor;
    if (g.floorAt(0, 0, 0.5f, 0.6f, 3.0f, originFloor) && g.walkable(0, 0, originFloor)) { aim[0] = 0; aim[1] = originFloor; aim[2] = 0; }
    // the nearest level spot to it with the room it aims for (as much as the area has, on a small one)
    const int wantCm = std::min(static_cast<int>(kWantClear * 100), most * 4 / 5);
    float best = 1e30f;
    bool found = false;
    for (int z = 0; z < g.nz; ++z)
        for (int x = 0; x < g.nx; ++x)
            for (const WalkSpan* s = g.begin(x, z); s != g.end(x, z); ++s) {
                if (!(s->flags & kWalkOk) || s->dist < wantCm || !levelAround(g, x, z, *s)) continue;
                const float d = std::hypot(g.centerX(x) - aim[0], g.centerZ(z) - aim[2]) + 2 * std::fabs(s->hi - aim[1]);
                if (d < best) { best = d; pos[0] = g.centerX(x); pos[1] = s->hi; pos[2] = g.centerZ(z); found = true; }
            }
    // A home floating in space (no floor under its origin, and nothing to stand on near it: the only "floor" the area
    // found is the top of a far bubble or rock) is seen from its origin, floating, as its makers made it.
    float floorUnderOrigin;
    const bool originGrounded = g.floorAt(0, 0, 0.5f, 0.6f, 3.0f, floorUnderOrigin);
    if (!originGrounded && (!found || std::hypot(pos[0], pos[2]) > 6.0f || std::fabs(pos[1]) > 2.0f)) {
        pos[0] = pos[1] = pos[2] = 0;
        yaw = bestFacing(g, 0, 0, 0);
        return true;
    }
    if (!found) return false;
    yaw = bestFacing(g, pos[0], pos[1], pos[2]);
    return true;
}

}  // namespace spawnpick
