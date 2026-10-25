#pragma once
// Walking with any "down" (WorldGravity zones): onto walls, across ceilings, all the way round a planet, and falling.
// Frame Home uses GravityWalker for homes that have gravity zones; without zones Locomotion keeps its upright walk.
//
// The rig (worldFromTracking) is a full frame here: tracking +Y is the player's up, -Z their facing, origin on the
// floor under the tracking origin. Each frame:
//   1. turns (snap or smooth: the caller's per-frame angle) rotate about up, through the head
//   2. up eases toward the zone's (Direction: the zone's +Y; Planet: away from the centre; Surface: out of the
//      ground underfoot, or out of a wall walked into), turning about the feet so you roll onto the new floor
//   3. the stick walks along the ground plane; walls (steep for this up) push back, except in Surface zones,
//      where walking into a wall makes it the floor
//   4. a ray down (along -up) finds the ground: stand on it (ramps followed exactly, steps eased), or fall with
//      gravity and land; falling further than the world's fallLimit puts you back at the spawn
// Walking speeds up and slows down over a fraction of a second (grav::approach), never instantly.
#include "core/world.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace grav {

struct V {
    float x = 0, y = 0, z = 0;
    V() = default;
    V(float a, float b, float c) : x(a), y(b), z(c) {}
    explicit V(const float* p) : x(p[0]), y(p[1]), z(p[2]) {}
    V operator+(const V& o) const { return {x + o.x, y + o.y, z + o.z}; }
    V operator-(const V& o) const { return {x - o.x, y - o.y, z - o.z}; }
    V operator*(float s) const { return {x * s, y * s, z * s}; }
    V operator-() const { return {-x, -y, -z}; }
};
inline float dot(const V& a, const V& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V cross(const V& a, const V& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(const V& a) { return std::sqrt(dot(a, a)); }
inline V norm(const V& a) { const float l = len(a); return l > 1e-9f ? a * (1.0f / l) : V(0, 1, 0); }
// Rotates v about unit axis k by angle a (Rodrigues).
inline V rotate(const V& v, const V& k, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1 - c));
}
inline V xform(const float* m, const V& p) {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13], m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}
inline V closestOnTri(const V& p, const V& a, const V& b, const V& c) {   // Ericson, Real-Time Collision Detection 5.1.5
    const V ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    const V bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return a + ab * (d1 / (d1 - d3));
    const V cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float den = 1 / (va + vb + vc);
    return a + ab * (vb * den) + ac * (vc * den);
}

// Walking velocity eased toward the stick's: full speed in kAccel s from a standstill, a stop in kDecel s. Turning
// (the head, mid-walk) rides the same rate, so the path curves instead of kinking. Instant starts and stops are a
// classic comfort problem in VR; a short ramp reads as natural without feeling floaty.
constexpr float kAccel = 0.15f, kDecel = 0.12f;
inline void approach(V& v, const V& want, float maxSpeed, float dt) {
    const V dv = want - v;
    const float l = len(dv);
    if (l < 1e-6f) { v = want; return; }
    const float rate = maxSpeed / (dot(want, want) > dot(v, v) ? kAccel : kDecel);
    v = v + dv * (std::min(l, rate * dt) / l);
}
// Critically damped easing of x toward target (Unity's SmoothDamp): starts and ends with no jolt, never overshoots.
// vel carries over between frames.
inline float smoothDamp(float x, float target, float& vel, float smoothTime, float dt) {
    const float o = 2 / smoothTime, k = o * dt, e = 1 / (1 + k + 0.48f * k * k + 0.235f * k * k * k);
    const float ch = x - target, tmp = (vel + o * ch) * dt;
    vel = (vel - o * tmp) * e;
    float out = target + (ch + tmp) * e;
    if ((target - x > 0) == (out > target)) { out = target; vel = 0; }
    return out;
}
// Steps vs ramps: a change of floor height within this of the distance walked is a slope, followed exactly (easing
// it would sink you into a ramp going up); more is a step, eased up (kStepUp s) or down (kStepDown s).
constexpr float kRampSlope = 1.2f, kStepUp = 0.09f, kStepDown = 0.11f;
// The walk stick: a round deadzone (no axis snapping), then n^1.6 so a light push strolls and only a full one
// walks at full speed.
inline void shapeStick(float x, float y, float& ox, float& oy) {
    const float m = std::sqrt(x * x + y * y);
    if (m < 0.15f) { ox = oy = 0; return; }
    const float n = std::min(1.0f, (m - 0.15f) / 0.85f), k = std::pow(n, 1.6f) / m;
    ox = x * k; oy = y * k;
}

// Every surface of the world in a 3D grid, queried in any direction (WorldRuntime only looks straight down).
class Surfaces {
public:
    struct Tri { V a, b, c, n; float floorUp; bool blocks; bool allWall; };   // floorUp: |n.up| needed to stand (0 = never floor);
                                                                            // allWall: every face blocks (the Wall role)
    struct Ball { V c; float r; float floorUp; bool blocks; };

    void build(const HomeWorld& w) {
        tris_.clear(); balls_.clear(); grid_.clear(); big_.clear();
        auto addTri = [&](const V& a, const V& b, const V& c, float floorUp, bool blocks, bool allWall = false) {
            const V n = cross(b - a, c - a);
            if (len(n) < 1e-9f) return;
            Tri t{a, b, c, norm(n), floorUp, blocks, allWall};
            const uint32_t id = static_cast<uint32_t>(tris_.size());
            tris_.push_back(t);
            const int x0 = cell(std::min({a.x, b.x, c.x})), x1 = cell(std::max({a.x, b.x, c.x}));
            const int y0 = cell(std::min({a.y, b.y, c.y})), y1 = cell(std::max({a.y, b.y, c.y}));
            const int z0 = cell(std::min({a.z, b.z, c.z})), z1 = cell(std::max({a.z, b.z, c.z}));
            if (static_cast<int64_t>(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 4096) { big_.push_back(id); return; }
            for (int x = x0; x <= x1; ++x) for (int y = y0; y <= y1; ++y) for (int z = z0; z <= z1; ++z) grid_[key(x, y, z)].push_back(id);
        };
        if (!w.walkIndices.empty()) {
            const size_t nv = w.walkPositions.size() / 3;
            for (size_t i = 0; i + 2 < w.walkIndices.size(); i += 3)
                if (w.walkIndices[i] < nv && w.walkIndices[i + 1] < nv && w.walkIndices[i + 2] < nv)
                    addTri(V(&w.walkPositions[w.walkIndices[i] * 3]), V(&w.walkPositions[w.walkIndices[i + 1] * 3]), V(&w.walkPositions[w.walkIndices[i + 2] * 3]), 0.01f, false);
        }
        for (const WorldCollider& c : w.colliders) {
            if (c.role == WorldCollider::Trigger) continue;
            const float floorUp = c.role == WorldCollider::Solid ? 0.7f : c.role == WorldCollider::Floor || c.role == WorldCollider::Walkable ? 0.5f : 0.0f;
            const bool blocks = c.role == WorldCollider::Solid || c.role == WorldCollider::Wall || c.role == WorldCollider::Object;
            const bool allWall = c.role == WorldCollider::Wall || c.role == WorldCollider::Object;   // blocks at any slope
            if (c.kind == WorldCollider::Mesh) {
                const std::vector<float> p = worldMeshPositions(c);
                const size_t nv = p.size() / 3;
                for (size_t i = 0; i + 2 < c.indices.size(); i += 3)
                    if (c.indices[i] < nv && c.indices[i + 1] < nv && c.indices[i + 2] < nv)
                        addTri(V(&p[c.indices[i] * 3]), V(&p[c.indices[i + 1] * 3]), V(&p[c.indices[i + 2] * 3]), floorUp, blocks, allWall);
            } else if (c.kind == WorldCollider::Box) {
                V v[8];
                for (int k = 0; k < 8; ++k) v[k] = xform(c.world, V((k & 1 ? 1 : -1) * c.half[0], (k & 2 ? 1 : -1) * c.half[1], (k & 4 ? 1 : -1) * c.half[2]));
                static const int f[6][4] = {{0, 1, 3, 2}, {4, 6, 7, 5}, {0, 4, 5, 1}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 5, 7, 3}};
                for (const auto& q : f) { addTri(v[q[0]], v[q[1]], v[q[2]], floorUp, blocks, allWall); addTri(v[q[0]], v[q[2]], v[q[3]], floorUp, blocks, allWall); }
            } else if (c.kind == WorldCollider::Sphere) {
                balls_.push_back({xform(c.world, V(0, 0, 0)), c.radius, floorUp, blocks});
            } else {   // capsule: three balls along its axis
                for (float t : {-0.5f, 0.0f, 0.5f}) balls_.push_back({xform(c.world, V(0, t * c.length, 0)), c.radius, floorUp, blocks});
            }
        }
        stamp_.assign(tris_.size(), 0);
    }

    bool empty() const { return tris_.empty() && balls_.empty(); }

    // First surface you could stand on along o + d t (d unit), t in [0, maxT]. anyAngle: every surface counts as
    // ground (Surface zones). n comes back facing the ray's origin.
    // standableOut: when given, every surface stops the ray (a teleport arc doesn't pass through walls and
    // ceilings), and it says whether the one hit can be stood on.
    bool ray(const V& o, const V& d, float maxT, const V& up, bool anyAngle, float& tHit, V& nHit, bool* standableOut = nullptr) const {
        bool hit = false;
        tHit = maxT;
        auto test = [&](uint32_t id) {
            const Tri& t = tris_[id];
            const V facing = dot(t.n, d) < 0 ? t.n : -t.n;   // the side the ray comes from
            const bool ok = standable(t.floorUp, facing, up, anyAngle);
            if (!ok && !standableOut) return;
            float th;
            if (rayTri(o, d, t, th) && th >= 0 && th <= tHit) { tHit = th; nHit = facing; hit = true; if (standableOut) *standableOut = ok; }
        };
        ++gen_;
        const float step = kCell * 0.5f;
        for (float s = 0; s <= maxT + step; s += step) {
            const V p = o + d * std::min(s, maxT);
            const int cx = cell(p.x), cy = cell(p.y), cz = cell(p.z);
            for (int x = cx - 1; x <= cx + 1; ++x) for (int y = cy - 1; y <= cy + 1; ++y) for (int z = cz - 1; z <= cz + 1; ++z) {
                auto it = grid_.find(key(x, y, z));
                if (it == grid_.end()) continue;
                for (uint32_t id : it->second) if (stamp_[id] != gen_) { stamp_[id] = gen_; test(id); }
            }
            if (s > tHit) break;
        }
        for (uint32_t id : big_) test(id);
        for (const Ball& b : balls_) {
            const V oc = o - b.c;
            const float bq = dot(oc, d), cq = dot(oc, oc) - b.r * b.r, disc = bq * bq - cq;
            if (disc < 0) continue;
            const float th = -bq - std::sqrt(disc);
            if (th < 0 || th > tHit) continue;
            const V n = norm(o + d * th - b.c);
            const bool ok = standable(b.floorUp, n, up, anyAngle);
            if (!ok && !standableOut) continue;
            tHit = th; nHit = n; hit = true;
            if (standableOut) *standableOut = ok;
        }
        return hit;
    }

    // Pushes a ball (the body) out of what blocks it for this up: steep blocking surfaces, blocking balls.
    // onlyWalls: just the surfaces that can never be floor (Surface zones, where every other surface is walkable).
    V pushOut(V p, float r, const V& up, bool onlyWalls = false) const {
        for (int pass = 0; pass < 2; ++pass) {
            ++gen_;
            const int cx = cell(p.x), cy = cell(p.y), cz = cell(p.z);
            auto test = [&](uint32_t id) {
                const Tri& t = tris_[id];
                if (!t.blocks || (onlyWalls ? t.floorUp > 0 : !t.allWall && std::fabs(dot(t.n, up)) >= 0.7f)) return;
                const V q = closestOnTri(p, t.a, t.b, t.c), dv = p - q;
                const float dl = len(dv);
                if (dl < r && dl > 1e-6f) p = p + dv * ((r - dl) / dl);
            };
            for (int x = cx - 1; x <= cx + 1; ++x) for (int y = cy - 1; y <= cy + 1; ++y) for (int z = cz - 1; z <= cz + 1; ++z) {
                auto it = grid_.find(key(x, y, z));
                if (it == grid_.end()) continue;
                for (uint32_t id : it->second) if (stamp_[id] != gen_) { stamp_[id] = gen_; test(id); }
            }
            for (uint32_t id : big_) test(id);
        }
        for (const Ball& b : balls_) {
            if (!b.blocks || (onlyWalls && b.floorUp > 0)) continue;
            const V dv = p - b.c;
            const float dl = len(dv);
            if (dl < b.r + r && dl > 1e-6f) p = b.c + dv * ((b.r + r) / dl);
        }
        return p;
    }

private:
    static constexpr float kCell = 2.0f;
    std::vector<Tri> tris_;
    std::vector<Ball> balls_;
    std::unordered_map<int64_t, std::vector<uint32_t>> grid_;
    std::vector<uint32_t> big_;
    mutable std::vector<uint32_t> stamp_;
    mutable uint32_t gen_ = 0;

    // (clamped, NaN as 0: a damaged file's 1e30 or NaN vertex must not overflow the ints the cell loops count in)
    static int cell(float v) {
        const float c = std::floor(v / kCell);
        return c > -1e6f && c < 1e6f ? static_cast<int>(c) : c >= 1e6f ? 1000000 : c <= -1e6f ? -1000000 : 0;
    }
    static int64_t key(int x, int y, int z) { return (static_cast<int64_t>(x & 0x1FFFFF) << 42) | (static_cast<int64_t>(y & 0x1FFFFF) << 21) | (z & 0x1FFFFF); }
    // n: the surface's normal on the side being approached. Facing up (for this up) to stand on it: the underside
    // of a ceiling or a beam is never floor.
    static bool standable(float floorUp, const V& n, const V& up, bool anyAngle) {
        if (floorUp <= 0) return false;                        // walls never carry you
        return anyAngle || dot(n, up) > floorUp;
    }
    static bool rayTri(const V& o, const V& d, const Tri& t, float& th) {
        const V e1 = t.b - t.a, e2 = t.c - t.a, pv = cross(d, e2);
        const float det = dot(e1, pv);
        if (std::fabs(det) < 1e-9f) return false;
        const float inv = 1 / det;
        const V tv = o - t.a;
        const float u = dot(tv, pv) * inv;
        if (u < 0 || u > 1) return false;
        const V qv = cross(tv, e1);
        const float v = dot(d, qv) * inv;
        if (v < 0 || u + v > 1) return false;
        th = dot(e2, qv) * inv;
        return true;
    }
};

// The zone deciding down at p: the highest priority one containing it (ties: the later), or -1.
inline int zoneAt(const std::vector<WorldGravity>& zones, const V& p) {
    int best = -1;
    for (size_t i = 0; i < zones.size(); ++i) {
        const WorldGravity& g = zones[i];
        bool in;
        if (g.kind == WorldGravity::Planet) in = len(p - V(&g.world[12])) <= g.radius;
        else {
            const V d = p - V(&g.world[12]);
            in = true;
            for (int k = 0; k < 3 && in; ++k) {
                const V col(&g.world[k * 4]);
                const float sc = len(col);                       // a scaled frame scales the region (as Studio draws it)
                in = sc > 1e-6f && std::fabs(dot(d, col * (1.0f / sc))) <= g.half[k] * sc;
            }
        }
        if (in && (best < 0 || g.priority >= zones[best].priority)) best = static_cast<int>(i);
    }
    return best;
}

}  // namespace grav

class GravityWalker {
public:
    using V = grav::V;

    void build(const HomeWorld& w) {
        surf_.build(w);
        zones_ = w.gravity;
        fallLimit_ = w.fallLimit > 1 ? w.fallLimit : 30.0f;
        hasSpawn_ = !w.spawns.empty();
        if (hasSpawn_) spawn_ = w.spawns[0];
        reset();
    }

    // To the spawn (or the origin), standing on the ground under it, facing its way.
    void reset() {
        V p = hasSpawn_ ? V(spawn_.pos) : V(0, 0, 0);
        const V up = upAt(p, V(0, 1, 0));                    // not the old up: you may have fallen off a wall
        const float yaw = hasSpawn_ ? spawn_.yaw : 0.0f;
        V fwd(-std::sin(yaw), 0, -std::cos(yaw));
        fwd = fwd - up * grav::dot(fwd, up);
        if (grav::len(fwd) < 1e-3f) fwd = grav::cross(up, std::fabs(up.x) < 0.9f ? V(1, 0, 0) : V(0, 0, 1));
        setBasis(up, grav::norm(fwd));
        float t; V n;
        if (surf_.ray(p + up * 1.5f, -up, 6.0f, up, zoneKind(p) == WorldGravity::Surface, t, n)) p = p + up * 1.5f - up * t;
        origin_ = p;
        vel_ = fallen_ = 0;
        falling_ = false;
        wallUp_ = V();
        wallTime_ = 0;
        walkV_ = V();
        rem_ = riseV_ = 0;
        lastFeet_ = origin_;
    }

    void rig(float m[16]) const {
        const float r[16] = {x_.x, x_.y, x_.z, 0, up_.x, up_.y, up_.z, 0, z_.x, z_.y, z_.z, 0, origin_.x, origin_.y, origin_.z, 1};
        std::copy(r, r + 16, m);
    }
    V up() const { return up_; }
    bool falling() const { return falling_; }
    int respawns() const { return respawns_; }
    V head(const float headT[16]) const { return toWorld(V(&headT[12])); }
    void setSpeed(float metresPerSecond) { speed_ = metresPerSecond; }   // the walk stick at full deflection

    // One frame: moveX / moveY the walk stick (-1..1), turn the turn to make now (radians, + = left).
    void update(float dt, const float headT[16], float moveX, float moveY, float turn) {
        dt = std::min(dt, 0.05f);
        const V h(&headT[12]);
        V hw = toWorld(h);
        if (turn != 0) rotateRig(up_, turn, hw);
        // Up: the zone's, or the wall walked into (Surface zones), eased so you roll onto it.
        const int zi = grav::zoneAt(zones_, hw);
        const bool surfaceZone = zi >= 0 && zones_[zi].kind == WorldGravity::Surface;
        V target = upAt(hw, up_);
        if (surfaceZone) {
            float t; V n;
            if (surf_.ray(hw, -up_, h.y + 1.0f, up_, true, t, n)) target = n;
            if (wallTime_ > 0) { target = wallUp_; wallTime_ -= dt; }
        }
        const float ang = std::acos(std::clamp(grav::dot(up_, target), -1.0f, 1.0f));
        if (ang > 1e-3f) {
            V axis = grav::cross(up_, target);
            if (grav::len(axis) < 1e-5f) axis = x_;            // straight flip: roll forward
            const float stepA = std::min(ang, (surfaceZone ? 4.0f : 2.5f) * dt);
            rotateRig(grav::norm(axis), stepA, hw - up_ * h.y);  // about the feet
        }
        hw = toWorld(h);
        // Walk along the ground plane, head-relative, the velocity eased (approach) toward the stick's.
        {
            V want;
            V fwd = toWorldDir(V(-headT[8], -headT[9], -headT[10]));
            fwd = fwd - up_ * grav::dot(fwd, up_);
            if ((moveX != 0 || moveY != 0) && grav::len(fwd) > 1e-4f) {
                fwd = grav::norm(fwd);
                want = (fwd * moveY + grav::cross(fwd, up_) * moveX) * speed_;
            }
            walkV_ = walkV_ - up_ * grav::dot(walkV_, up_);         // along the ground as up turns
            grav::approach(walkV_, want, speed_, dt);
        }
        if (grav::len(walkV_) > 1e-5f) {
            V d = walkV_ * dt;
            const V feet = hw - up_ * h.y;
            if (surfaceZone) {
                // A wall ahead becomes the floor: start rolling onto it.
                float t; V n;
                const V dd = grav::norm(d);
                if (surf_.ray(feet + up_ * 0.3f, dd, grav::len(d) + 0.35f, up_, true, t, n) && std::fabs(grav::dot(n, up_)) < 0.7f) {
                    wallUp_ = n;
                    wallTime_ = 0.6f;
                }
                // Walls that can never be floor (the Wall role) still block here.
                const V body = feet + up_ * 0.9f;
                d = surf_.pushOut(body + d, 0.25f, up_, true) - body;
                d = d - up_ * grav::dot(d, up_);
            } else {
                const V body = feet + up_ * 0.9f;
                V moved = surf_.pushOut(body + d, 0.25f, up_);
                d = moved - body;
                d = d - up_ * grav::dot(d, up_);
            }
            origin_ = origin_ + d;
            hw = hw + d;
            if (dt > 0) walkV_ = d * (1 / dt);                      // what a wall left of it: no pent-up push (a 0 s frame: NaN)
        }
        // Stand on the ground, or fall. The ground is looked for from knee height (a step), not from the head: a
        // head inside a cabinet or under a beam would otherwise find that and pop you up onto it.
        float t; V n;
        const V feet = hw - up_ * h.y;
        constexpr float kStep = 0.45f;
        V walked = feet - lastFeet_;                             // stick and roomscale, along the ground
        walked = walked - up_ * grav::dot(walked, up_);
        lastFeet_ = feet;
        if (!falling_ && surf_.ray(feet + up_ * kStep, -up_, kStep + 0.6f, up_, surfaceZone, t, n)) {
            float rise = kStep - t;                              // + : the ground is above the feet (a step up)
            // The ground's own change since last frame: a ramp is followed exactly, a step joins the eased rest.
            const float d = rise - rem_;
            if (std::fabs(d) <= grav::len(walked) * grav::kRampSlope + 0.004f) { origin_ = origin_ + up_ * d; rise -= d; }
            const float left = grav::smoothDamp(rise, 0, riseV_, rise > 0 ? grav::kStepUp : grav::kStepDown, dt);
            origin_ = origin_ + up_ * (rise - left);
            rem_ = left;
            vel_ = fallen_ = 0;
        } else {
            rem_ = riseV_ = 0;
            falling_ = true;
            vel_ = std::min(vel_ + 9.8f * dt, 55.0f);
            const float stepD = vel_ * dt;
            // Landing: from a step above the feet, so ground rising under you while you fall is still found.
            if (surf_.ray(feet + up_ * kStep, -up_, stepD + kStep, up_, surfaceZone, t, n)) {
                origin_ = origin_ + up_ * (kStep - t);
                falling_ = false;
                vel_ = fallen_ = 0;
            } else {
                origin_ = origin_ - up_ * stepD;
                fallen_ += stepD;
                if (fallen_ > fallLimit_) { ++respawns_; reset(); }
            }
        }
    }

    // The teleport arc from the hand (tracking space), under this gravity; true with the landing point. It stops at
    // the first surface it meets: one you can stand on is the landing, anything else (a wall, a ceiling) means
    // nowhere. normal (optional): the landing surface's normal, facing the arc. path (optional): the arc's points
    // in world space, hand first, for drawing it. headPos (optional, tracking space): a hand reached through a wall
    // from the head aims nothing.
    bool aim(const float handPos[3], const float handDir[3], float target[3], float normal[3] = nullptr,
             std::vector<float>* path = nullptr, const float* headPos = nullptr) const {
        V p = toWorld(V(handPos)), v = toWorldDir(V(handDir)) * 7.0f;
        auto keep = [&](const V& q) { if (path) { path->push_back(q.x); path->push_back(q.y); path->push_back(q.z); } };
        if (path) path->clear();
        keep(p);
        const bool anyAngle = zoneKind(p) == WorldGravity::Surface;
        if (headPos) {
            const V hd = toWorld(V(headPos)), to = p - hd;
            const float tl = grav::len(to);
            float t; V n; bool st = false;
            if (tl > 1e-4f && surf_.ray(hd, to * (1 / tl), tl, up_, anyAngle, t, n, &st)) return false;
        }
        for (int i = 0; i < 90; ++i) {
            const float dt = 0.03f;
            const V seg = v * dt - up_ * (4.9f * dt * dt);
            const float sl = grav::len(seg);
            float t; V n;
            bool canStand = false;
            if (sl > 1e-6f && surf_.ray(p, seg * (1 / sl), sl, up_, anyAngle, t, n, &canStand)) {
                const V hit = p + seg * (t / sl);
                keep(hit);
                if (!canStand) return false;                    // a wall or a ceiling in the way: nowhere to land
                target[0] = hit.x; target[1] = hit.y; target[2] = hit.z;
                if (normal) { normal[0] = n.x; normal[1] = n.y; normal[2] = n.z; }
                return true;
            }
            p = p + seg;
            keep(p);
            v = v - up_ * (9.8f * dt);
        }
        return false;
    }

    // Moves the player so their feet land on `target` (world). In a Surface zone, `normal` (the landing surface's)
    // becomes the new up there: teleporting onto a wall stands you on it.
    void teleport(const float target[3], const float headT[16], const float* normal = nullptr) {
        if (normal && zoneKind(V(target)) == WorldGravity::Surface) {
            const V nu = grav::norm(V(normal));
            const float ang = std::acos(std::clamp(grav::dot(up_, nu), -1.0f, 1.0f));
            if (ang > 1e-3f) {
                V axis = grav::cross(up_, nu);
                if (grav::len(axis) < 1e-5f) axis = x_;
                rotateRig(grav::norm(axis), ang, toWorld(V(&headT[12])) - up_ * headT[13]);
            }
            wallTime_ = 0;
        }
        const V feet = toWorld(V(&headT[12])) - up_ * headT[13];
        origin_ = origin_ + (V(target) - feet);
        falling_ = false;
        vel_ = fallen_ = 0;
        walkV_ = V();                                            // you arrive standing still
        rem_ = riseV_ = 0;
        lastFeet_ = V(target);
    }

    // tracking -> world, for overlays (the teleport ring): the ring's frame at a world point.
    V toTracking(const V& w) const { const V d = w - origin_; return V(grav::dot(d, x_), grav::dot(d, up_), grav::dot(d, z_)); }

private:
    grav::Surfaces surf_;
    std::vector<WorldGravity> zones_;
    float fallLimit_ = 30, speed_ = 2.0f;
    bool hasSpawn_ = false;
    WorldPoint spawn_;
    V x_{1, 0, 0}, up_{0, 1, 0}, z_{0, 0, 1}, origin_;
    float vel_ = 0, fallen_ = 0;
    bool falling_ = false;
    V wallUp_;
    float wallTime_ = 0;
    int respawns_ = 0;
    V walkV_;                                                // walking velocity (eased, along the ground)
    float rem_ = 0, riseV_ = 0;                              // a step still being eased (metres along up), its rate
    V lastFeet_;

    V toWorld(const V& t) const { return x_ * t.x + up_ * t.y + z_ * t.z + origin_; }
    V toWorldDir(const V& t) const { return x_ * t.x + up_ * t.y + z_ * t.z; }
    void setBasis(const V& up, const V& fwd) { up_ = grav::norm(up); z_ = -fwd; x_ = grav::norm(grav::cross(up_, z_)); z_ = grav::cross(x_, up_); }
    void rotateRig(const V& axis, float a, const V& pivot) {
        up_ = grav::norm(grav::rotate(up_, axis, a));
        x_ = grav::rotate(x_, axis, a);
        x_ = grav::norm(x_ - up_ * grav::dot(x_, up_));      // Gram-Schmidt: no drift from many small turns
        z_ = grav::cross(x_, up_);
        origin_ = pivot + grav::rotate(origin_ - pivot, axis, a);
    }
    int zoneKind(const V& p) const { const int zi = grav::zoneAt(zones_, p); return zi >= 0 ? zones_[zi].kind : -1; }
    // Up at p for Direction / Planet zones and no zone; Surface zones keep `cur` (their up comes from the ground).
    V upAt(const V& p, const V& cur) const {
        const int zi = grav::zoneAt(zones_, p);
        if (zi < 0) return V(0, 1, 0);
        const WorldGravity& g = zones_[zi];
        if (g.kind == WorldGravity::Direction) return grav::norm(V(&g.world[4]));
        if (g.kind == WorldGravity::Planet) { const V d = p - V(&g.world[12]); return grav::len(d) > 1e-3f ? grav::norm(d) : cur; }
        return cur;
    }
};
