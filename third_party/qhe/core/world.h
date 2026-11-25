#pragma once
// A home's world layer: where the player stands and can go — the parts of Meta's environment system that are not
// drawn. Converted from Meta's components (SpawnPoint, AvatarSitting, LocomotionHotspot, Collider Box / Sphere /
// Capsule / Mesh / ConvexMesh) into one plain format that a .fenv stores as world.json, Frame Studio
// edits and Frame Home uses for the floor, the start position, walking, teleporting and sitting.
// Everything is in world space, metres, +Y up.
#include "core/tinyjson.h"
#include "core/types.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

struct WorldPoint {                     // a spawn, a seat or a teleport hotspot
    std::string name;
    float pos[3] = {0, 0, 0};           // on the floor (seats: the seat position)
    float yaw = 0;                      // radians about +Y; 0 faces -Z
    float exit[3] = {0, 0, 0};          // seats: where standing up puts the player (relative to pos)
    bool hidden = false;                // Frame Studio's view only (Frame Home ignores it)
};

struct WorldCollider {
    enum Kind { Box = 0, Sphere = 1, Capsule = 2, Mesh = 3 };
    // What the surface does for the player. Roles only add: a walkable area never switches other floors off.
    enum Role {
        Solid = 0,      // a surface: walk on it where it's flat enough; too steep to walk up, it's a wall (Meta's
                        // colliders, geometry, Studio's surfaces)
        Floor = 1,      // older files only: read as Solid (one kind of surface)
        Wall = 2,       // older files only: read as Solid; still made by Auto-generate's opt-in extras (world_gen.h)
        Trigger = 3,    // detects, doesn't block or carry (Meta's trigger bodies)
        Object = 4,     // a thing's collider: blocks you (you don't walk through it), never stood on (Studio's Collider)
        Walkable = 5,   // a floor drawn on a thing (Studio's Draw Floor): its faces that face up are always floor, kept
                        // even on furniture; never blocks (the thing itself still does)
        Navmesh = 6,    // a navmesh: the walkable area itself, as a mesh (made from the baked area or drawn in Studio, its
                        // corners editable). A home with any is walked on its navmeshes alone: exactly their faces, at
                        // their heights, their edges where you stop; nothing else decides (HomeWorld::hasNavmesh)
    };
    std::string name;
    int kind = Box;
    int role = Solid;
    bool simplified = false;            // a mesh simplified already (simplifyWorldMeshes; a bake stores them so; not saved in world.json)
    float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // shape frame (column-major); meshes too
    float half[3] = {0.5f, 0.5f, 0.5f};                                   // box half extents (local)
    float radius = 0.5f, length = 0;                                      // sphere / capsule (length along local Y)
    std::vector<float> positions;       // mesh colliders: triangles in the shape frame (identity = world space)
    std::vector<u32> indices;
    std::string mesh;                   // made from this home object (Studio keeps it following the object)
    bool hidden = false;                // Frame Studio's view only
    bool placed = false;                // placed in Frame Studio (not the home's, not from an object): overlapping
                                        // placed surfaces are combined into one
    bool home = false;                  // the home's own collision, as its makers set it up (Meta's colliders): a home
                                        // with some walks on them, not on its drawn meshes (HomeWorld::hasHomeCollision)
    std::vector<float> boxes;           // a combined surface: the boxes it's the outside of, 19 floats each (16 frame,
                                        // 3 half extents), in its own frame (Studio's, to combine it again exactly)
    bool trigger() const { return role == Trigger; }
};

// Which way is down. Without zones it's -Y everywhere (Meta's homes); a zone changes it inside its region:
//   Direction: down is the zone frame's -Y — a room where a wall is the floor, a ceiling walk
//   Planet:    down points at the zone's centre, within `radius` of it — walk all the way round a sphere
//   Surface:   down is into whatever surface is under your feet — walk off the floor up a wall, onto the ceiling
// The highest priority zone containing the player wins (ties: the later one).
struct WorldGravity {
    enum Kind { Direction = 0, Planet = 1, Surface = 2 };
    std::string name;
    int kind = Direction;
    float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};   // region frame (Planet: its centre)
    float half[3] = {5, 5, 5};          // box region (Direction, Surface), local half extents
    float radius = 30;                  // Planet: how far its pull reaches from the centre
    int priority = 0;
    bool hidden = false;                // Frame Studio's view only
};

// A stroke of Studio's Draw Floor: the walkable area's floor inside it, on whatever surface is there, is floor
// (painted) or not (erased), whatever the bake would say. In order: a later stroke over an earlier one wins.
// Its shape: a dab of the round or the square brush, a rectangle or a polygon. The floor it's on is the one it
// touched at pos (and what joins that a step or less apart, inside it); pos needn't be its middle.
struct WalkStroke {
    enum Shape { Round = 0, Square = 1, Rect = 2, Polygon = 3, kShapes };
    float pos[3] = {0, 0, 0};           // where it touched the surface (world)
    float radius = 0.3f;                // Round, Square: half across XZ (m), round pos
    bool erase = false;
    int shape = Round;
    float yaw = 0;                      // Square, Rect: turned about +Y (radians)
    float centre[2] = {0, 0};           // Rect: its middle (x, z)
    float half[2] = {0, 0};             // Rect: half its size along its own x and z (m)
    std::vector<float> outline;         // Polygon: its corners, x, z pairs

    // Inside it (XZ). slack: a round dab smaller than this still covers it (a tiny dab paints its own cell).
    bool covers(float x, float z, float slack = 0) const {
        switch (shape) {
            case Square: case Rect: {
                const float cx = shape == Rect ? centre[0] : pos[0], cz = shape == Rect ? centre[1] : pos[2];
                const float hx = shape == Rect ? half[0] : radius, hz = shape == Rect ? half[1] : radius;
                const float c = std::cos(yaw), s = std::sin(yaw), dx = x - cx, dz = z - cz;
                const float lx = dx * c - dz * s, lz = dx * s + dz * c;     // (its own frame)
                return std::fabs(lx) <= std::max(hx, slack) && std::fabs(lz) <= std::max(hz, slack);
            }
            case Polygon: {
                bool in = false;
                const size_t n = outline.size() / 2;
                for (size_t i = 0, j = n - 1; i < n; j = i++) {
                    const float xi = outline[i * 2], zi = outline[i * 2 + 1], xj = outline[j * 2], zj = outline[j * 2 + 1];
                    if ((zi > z) != (zj > z) && x < (xj - xi) * (z - zi) / (zj - zi) + xi) in = !in;
                }
                return in;
            }
            default: {
                const float dx = x - pos[0], dz = z - pos[2], r = std::max(radius, slack);
                return dx * dx + dz * dz <= r * r;
            }
        }
    }
    // Its XZ extent (lo x, lo z, hi x, hi z)
    void extent(float* lo, float* hi) const {
        lo[0] = hi[0] = pos[0]; lo[1] = hi[1] = pos[2];
        auto take = [&](float x, float z) { lo[0] = std::min(lo[0], x); lo[1] = std::min(lo[1], z); hi[0] = std::max(hi[0], x); hi[1] = std::max(hi[1], z); };
        if (shape == Polygon) { for (size_t i = 0; i + 1 < outline.size(); i += 2) take(outline[i], outline[i + 1]); return; }
        if (shape == Square || shape == Rect) {
            const float cx = shape == Rect ? centre[0] : pos[0], cz = shape == Rect ? centre[1] : pos[2];
            const float hx = shape == Rect ? half[0] : radius, hz = shape == Rect ? half[1] : radius;
            const float c = std::cos(yaw), s = std::sin(yaw);
            for (int k = 0; k < 4; ++k) {
                const float lx = k & 1 ? hx : -hx, lz = k & 2 ? hz : -hz;
                take(cx + lx * c + lz * s, cz - lx * s + lz * c);
            }
            return;
        }
        const float r = std::max(radius, 0.0f);
        take(pos[0] - r, pos[2] - r); take(pos[0] + r, pos[2] + r);
    }
    // Usable: in reach of a home (within 10 km of its middle, no side over 1 km, at most 4096 corners), and some size
    // (a polygon: three corners or more). A damaged or hand-made file's strokes past that are dropped when read.
    static constexpr float kFar = 10000, kBiggest = 1000;
    static constexpr size_t kMostCorners = 4096;
    bool valid() const {
        auto inReach = [](float v) { return std::isfinite(v) && std::fabs(v) <= kFar; };
        if (!inReach(pos[0]) || !inReach(pos[1]) || !inReach(pos[2]) || shape < 0 || shape >= kShapes || !std::isfinite(yaw)) return false;
        if (shape == Rect)
            return inReach(centre[0]) && inReach(centre[1]) && half[0] > 0 && half[1] > 0 && half[0] <= kBiggest && half[1] <= kBiggest;
        if (shape == Polygon) {
            if (outline.size() < 6 || outline.size() % 2 || outline.size() > 2 * kMostCorners) return false;
            for (float v : outline) if (!inReach(v)) return false;
            return true;
        }
        return radius > 0 && radius <= kBiggest;
    }
};

struct WalkGrid;                        // src/walkgrid.h: the walkable area, baked from the home's geometry and these colliders

struct HomeWorld {
    std::vector<WorldPoint> spawns;     // [0] = the player start
    std::vector<WorldPoint> seats;
    std::vector<WorldPoint> hotspots;
    std::vector<WorldCollider> colliders;
    std::vector<float> walkPositions;   // legacy walkable triangles (v1 files): read as one walkable-floor area
    std::vector<u32> walkIndices;
    std::vector<WorldGravity> gravity;  // empty: down is -Y everywhere
    std::vector<WalkStroke> walkPaint;  // the floor painted on / erased by hand (applied by every walkable-area bake)
    float fallLimit = 30;               // falling further than this (metres) puts you back at the spawn
    std::string origin;                 // "meta" (converted), "studio" (edited) or "" (none)
    bool walkNone = false;              // the navmesh (the walkable area) deleted: nothing is walkable, and it isn't made
                                        // again by itself (only by Frame Studio's Auto-generate, or a navmesh added)
    bool useHomeCollision = true;       // false: the home's own collision (Meta's floor and walls) is removed: it's walked
                                        // on its meshes instead (Frame Studio's choice; saved only when off)
    std::string walkGrid;               // the walkable area Frame Studio baked (walkgrid.h encoding); "": Frame Home bakes one at load
    std::shared_ptr<const WalkGrid> walkGridCache;   // walkGrid decoded, or one just built (never saved)
    // Whether the home's makers set up its collision (Meta's colliders): then that's what it's walked on, and what's
    // drawn without any (a vase, the leaves, a lamp) is walked through.
    bool hasNavmesh() const {
        for (const WorldCollider& c : colliders) if (c.role == WorldCollider::Navmesh && c.kind == WorldCollider::Mesh && !c.indices.empty()) return true;
        return false;
    }
    bool hasHomeCollision() const {
        if (!useHomeCollision) return false;
        for (const WorldCollider& c : colliders) if (c.home && c.role != WorldCollider::Trigger) return true;
        return false;
    }
    bool empty() const { return spawns.empty() && seats.empty() && hotspots.empty() && colliders.empty() && walkIndices.empty() && gravity.empty() && walkGrid.empty() && walkPaint.empty(); }
};

namespace world_detail {

inline void num(std::string& o, float v) { char b[32]; std::snprintf(b, sizeof b, "%.5g", std::isfinite(v) ? v : 0.0f); o += b; }
inline void arr(std::string& o, const float* v, size_t n) { o += '['; for (size_t i = 0; i < n; ++i) { if (i) o += ','; num(o, v[i]); } o += ']'; }
inline std::string q(const std::string& s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if (static_cast<unsigned char>(c) >= 0x20) o += c; }
    return o + "\"";
}
inline void point(std::string& o, const WorldPoint& p) {
    o += "{\"name\":" + q(p.name) + ",\"pos\":"; arr(o, p.pos, 3); o += ",\"yaw\":"; num(o, p.yaw); o += ",\"exit\":"; arr(o, p.exit, 3);
    if (p.hidden) o += ",\"hidden\":true";
    o += '}';
}
inline void readF(const tinyjson::Value& v, float* out, size_t n) { for (size_t i = 0; i < n && i < v.size(); ++i) out[i] = static_cast<float>(v[i].asFloat()); }
inline WorldPoint readPoint(const tinyjson::Value& v) {
    WorldPoint p; p.name = v["name"].asString(); readF(v["pos"], p.pos, 3); p.yaw = static_cast<float>(v["yaw"].asFloat()); readF(v["exit"], p.exit, 3);
    p.hidden = v["hidden"].boolVal;
    return p;
}

}  // namespace world_detail

// v1's free-floating walkable triangles become an ordinary walkable-floor area (selectable, movable, hideable).
inline void worldAdoptLegacyWalkable(HomeWorld& w) {
    if (w.walkIndices.empty()) return;
    WorldCollider c;
    c.name = "Walkable area";
    c.kind = WorldCollider::Mesh;
    c.role = WorldCollider::Solid;
    c.positions = std::move(w.walkPositions);
    c.indices = std::move(w.walkIndices);
    w.walkPositions.clear();
    w.walkIndices.clear();
    w.colliders.push_back(std::move(c));
}

// A collider's triangles in world space (mesh colliders are stored in their shape frame).
inline std::vector<float> worldMeshPositions(const WorldCollider& c) {
    std::vector<float> out(c.positions.size());
    const float* m = c.world;
    for (size_t v = 0; v + 2 < c.positions.size(); v += 3)
        for (int k = 0; k < 3; ++k)
            out[v + k] = m[k] * c.positions[v] + m[4 + k] * c.positions[v + 1] + m[8 + k] * c.positions[v + 2] + m[12 + k];
    return out;
}

inline std::string worldToJson(const HomeWorld& w) {
    using namespace world_detail;
    std::string o = "{\"format\":\"fenv-world\",\"version\":3,\"fallLimit\":" + [&] { std::string t; num(t, w.fallLimit); return t; }() + ",\"origin\":" + q(w.origin) + (w.useHomeCollision ? "" : ",\"useHomeCollision\":false") + (w.walkNone ? ",\"walkNone\":true" : "") + ",\n\"spawns\":[";
    for (size_t i = 0; i < w.spawns.size(); ++i) { if (i) o += ','; point(o, w.spawns[i]); }
    o += "],\n\"seats\":[";
    for (size_t i = 0; i < w.seats.size(); ++i) { if (i) o += ','; point(o, w.seats[i]); }
    o += "],\n\"hotspots\":[";
    for (size_t i = 0; i < w.hotspots.size(); ++i) { if (i) o += ','; point(o, w.hotspots[i]); }
    o += "],\n\"colliders\":[";
    for (size_t i = 0; i < w.colliders.size(); ++i) {
        const WorldCollider& c = w.colliders[i];
        o += std::string(i ? ",\n" : "\n") + "{\"name\":" + q(c.name) + ",\"kind\":" + std::to_string(c.kind) + ",\"role\":" + std::to_string(c.role) +
             ",\"trigger\":" + (c.trigger() ? "true" : "false") + (c.hidden ? ",\"hidden\":true" : "") + (c.placed ? ",\"placed\":true" : "") + (c.home ? ",\"home\":true" : "") +
             (c.mesh.empty() ? "" : ",\"mesh\":" + q(c.mesh)) + ",\"world\":";
        arr(o, c.world, 16); o += ",\"half\":"; arr(o, c.half, 3); o += ",\"radius\":"; num(o, c.radius); o += ",\"length\":"; num(o, c.length);
        if (!c.boxes.empty()) { o += ",\"boxes\":"; arr(o, c.boxes.data(), c.boxes.size()); }
        if (c.kind == WorldCollider::Mesh) {
            o += ",\"positions\":"; arr(o, c.positions.data(), c.positions.size());
            o += ",\"indices\":[";
            for (size_t k = 0; k < c.indices.size(); ++k) { if (k) o += ','; o += std::to_string(c.indices[k]); }
            o += ']';
        }
        o += '}';
    }
    o += "],\n\"walkable\":{\"positions\":"; arr(o, w.walkPositions.data(), w.walkPositions.size());
    o += ",\"indices\":[";
    for (size_t k = 0; k < w.walkIndices.size(); ++k) { if (k) o += ','; o += std::to_string(w.walkIndices[k]); }
    o += "]},\n\"gravity\":[";
    for (size_t i = 0; i < w.gravity.size(); ++i) {
        const WorldGravity& g = w.gravity[i];
        if (i) o += ',';
        o += "{\"name\":" + q(g.name) + ",\"kind\":" + std::to_string(g.kind) + ",\"world\":"; arr(o, g.world, 16);
        o += ",\"half\":"; arr(o, g.half, 3); o += ",\"radius\":"; num(o, g.radius); o += ",\"priority\":" + std::to_string(g.priority);
        if (g.hidden) o += ",\"hidden\":true";
        o += '}';
    }
    o += "]";
    if (!w.walkPaint.empty()) {        // x, y, z, radius, erase (0 / 1) per round dab
        o += ",\n\"walkPaint\":[";
        bool first = true, shaped = false;
        for (const WalkStroke& k : w.walkPaint) {
            if (k.shape != WalkStroke::Round) { shaped = true; continue; }
            if (!first) o += ',';
            first = false;
            const float v[5] = {k.pos[0], k.pos[1], k.pos[2], k.radius, k.erase ? 1.0f : 0.0f};
            for (int j = 0; j < 5; ++j) { if (j) o += ','; num(o, v[j]); }
        }
        o += ']';
        // Any other shape: every stroke, in order, as an object (a reader from before shapes takes the round dabs)
        if (shaped) {
            o += ",\n\"walkStrokes\":[";
            for (size_t i = 0; i < w.walkPaint.size(); ++i) {
                const WalkStroke& k = w.walkPaint[i];
                if (i) o += ',';
                o += "{\"shape\":" + std::to_string(k.shape) + ",\"pos\":"; arr(o, k.pos, 3);
                if (k.erase) o += ",\"erase\":true";
                if (k.shape == WalkStroke::Round || k.shape == WalkStroke::Square) { o += ",\"radius\":"; num(o, k.radius); }
                if (k.shape == WalkStroke::Square || k.shape == WalkStroke::Rect) { o += ",\"yaw\":"; num(o, k.yaw); }
                if (k.shape == WalkStroke::Rect) { o += ",\"centre\":"; arr(o, k.centre, 2); o += ",\"half\":"; arr(o, k.half, 2); }
                if (k.shape == WalkStroke::Polygon) { o += ",\"outline\":"; arr(o, k.outline.data(), k.outline.size()); }
                o += '}';
            }
            o += ']';
        }
    }
    if (!w.walkGrid.empty()) o += ",\n\"walkGrid\":" + q(w.walkGrid);
    return o + "}\n";
}

inline bool worldFromJson(const std::string& json, HomeWorld& w) {
    using namespace world_detail;
    try {
        const tinyjson::Value d = tinyjson::parse(json);
        if (d["format"].asString() != "fenv-world") return false;
        w = HomeWorld{};
        w.origin = d["origin"].asString();
        w.useHomeCollision = !d.has("useHomeCollision") || d["useHomeCollision"].boolVal;
        w.walkNone = d.has("walkNone") && d["walkNone"].boolVal;
        for (size_t i = 0; i < d["spawns"].size(); ++i) w.spawns.push_back(readPoint(d["spawns"][i]));
        for (size_t i = 0; i < d["seats"].size(); ++i) w.seats.push_back(readPoint(d["seats"][i]));
        for (size_t i = 0; i < d["hotspots"].size(); ++i) w.hotspots.push_back(readPoint(d["hotspots"][i]));
        for (size_t i = 0; i < d["colliders"].size(); ++i) {
            const auto& v = d["colliders"][i];
            WorldCollider c;
            c.name = v["name"].asString(); c.kind = static_cast<int>(v["kind"].asFloat());
            c.role = v.has("role") ? static_cast<int>(v["role"].asFloat()) : v["trigger"].boolVal ? WorldCollider::Trigger : WorldCollider::Solid;
            if (c.role != WorldCollider::Trigger && c.role != WorldCollider::Object && c.role != WorldCollider::Walkable && c.role != WorldCollider::Navmesh)
                c.role = WorldCollider::Solid;   // one kind of surface (Floor / Wall were older files')
            c.hidden = v["hidden"].boolVal; c.placed = v["placed"].boolVal; c.home = v["home"].boolVal; c.mesh = v["mesh"].asString();
            readF(v["world"], c.world, 16); readF(v["half"], c.half, 3);
            c.radius = static_cast<float>(v["radius"].asFloat()); c.length = static_cast<float>(v["length"].asFloat());
            c.boxes.resize(v["boxes"].size());
            readF(v["boxes"], c.boxes.data(), c.boxes.size());
            c.positions.resize(v["positions"].size());
            readF(v["positions"], c.positions.data(), c.positions.size());
            for (size_t k = 0; k < v["indices"].size(); ++k) c.indices.push_back(static_cast<u32>(v["indices"][k].asFloat()));
            w.colliders.push_back(std::move(c));
        }
        const auto& wk = d["walkable"];
        w.walkPositions.resize(wk["positions"].size());
        readF(wk["positions"], w.walkPositions.data(), w.walkPositions.size());
        for (size_t k = 0; k < wk["indices"].size(); ++k) w.walkIndices.push_back(static_cast<u32>(wk["indices"][k].asFloat()));
        for (size_t i = 0; i < d["gravity"].size(); ++i) {
            const auto& v = d["gravity"][i];
            WorldGravity g;
            g.name = v["name"].asString(); g.kind = static_cast<int>(v["kind"].asFloat());
            readF(v["world"], g.world, 16); readF(v["half"], g.half, 3);
            g.radius = static_cast<float>(v["radius"].asFloat()); g.priority = static_cast<int>(v["priority"].asFloat()); g.hidden = v["hidden"].boolVal;
            w.gravity.push_back(g);
        }
        if (d.has("walkStrokes")) {   // shapes: every stroke, in order
            const auto& ws = d["walkStrokes"];
            for (size_t i = 0; i < ws.size(); ++i) {
                const auto& v = ws[i];
                WalkStroke k;
                k.shape = static_cast<int>(v["shape"].asFloat());
                readF(v["pos"], k.pos, 3);
                k.erase = v["erase"].boolVal;
                if (v.has("radius")) k.radius = static_cast<float>(v["radius"].asFloat());
                if (v.has("yaw")) k.yaw = static_cast<float>(v["yaw"].asFloat());
                readF(v["centre"], k.centre, 2);
                readF(v["half"], k.half, 2);
                k.outline.resize(v["outline"].size());
                readF(v["outline"], k.outline.data(), k.outline.size());
                if (k.valid()) w.walkPaint.push_back(std::move(k));
            }
        } else {
            const auto& wp = d["walkPaint"];
            for (size_t i = 0; i + 4 < wp.size(); i += 5) {
                WalkStroke k;
                for (int j = 0; j < 3; ++j) k.pos[j] = static_cast<float>(wp[i + j].asFloat());
                k.radius = static_cast<float>(wp[i + 3].asFloat());
                k.erase = wp[i + 4].asFloat() > 0.5;
                if (k.valid()) w.walkPaint.push_back(k);
            }
        }
        w.walkGrid = d["walkGrid"].asString();
        if (d.has("fallLimit")) w.fallLimit = static_cast<float>(d["fallLimit"].asFloat());
        worldAdoptLegacyWalkable(w);
        return true;
    } catch (...) {
        return false;
    }
}
