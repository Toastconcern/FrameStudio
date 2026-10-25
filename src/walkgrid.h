#pragma once
// The walkable area ("walk grid"): where a standing player can be, baked from everything solid in a home the way
// Recast's heightfield does it, and shared by Frame Studio (which shows it and saves it in world.json) and Frame Home
// (which walks, falls and teleports with it: WorldRuntime prefers it to the raw colliders).
//   1. every solid triangle is rasterized into `cell` columns over XZ as height spans (what is solid at which
//      heights); a span's top is something to stand on where the triangle there faces up (within maxSlope)
//   2. a top is a floor when there's `headroom` over it: tables, shelves, chair seats and low plants block the floor
//      under them; neighbouring floors within `step` of each other connect (steps, ramps); higher is a ledge
//   3. seams and small holes up to `gap` across are bridged at the floor's height when nothing stands in them, so a
//      weird gap between two floor meshes never drops you
//   4. every floor gets its distance to the area's edge (Euclidean, not the cells' staircase): floors at least the
//      body `radius` in connect the area (a gap narrower than a body doesn't), and you stand right up to walls,
//      furniture and ledges (`inset` short of them: 0); the edge is a smooth curve (round furniture is round, a
//      slanted wall is a straight line to slide along)
//   5. what can't be reached from the start (spawns, hotspots, seats) is dropped: table tops, roofs, the inside of
//      walls; so are specks under minArea. Before that, the tops of things (a vase, a coffee table, a sofa: a small
//      level top higher than everything round it, peeled away again and again so a sofa's back, then its seat, then
//      its footrest go) are furniture, not floor: sat on, never walked on
//   6. stairs walk like a ramp: the height you walk at (walkY) is the floors' heights smoothed over ~25 cm; each tread
//      keeps its own height (hi) for landing on and drawing
// Roles override the shape: a Wall always blocks (any height, even below a step); a Floor is something to stand on
// up to 60 degrees and its areas are always kept; triggers (and objects marked Walk through) are left out.
// Queries answer for a standing body (the radius is baked in): floorAt, walkable, move (slides along edges), trace
// (teleport arcs: land on floors, stop at anything solid) and roomAt (nearest standing spot).
#include "core/world.h"
#include "miniz.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

struct WalkParams {
    float cell = 0.1f;          // column size (m)
    float step = 0.4f;          // highest step you walk up or down (m): a stair, a deck or a raised platform's edge
    float headroom = 1.6f;      // clear height a floor needs over it (m)
    float radius = 0.1f;        // body radius (m): a gap narrower than twice this is too narrow to get through
    float inset = 0;            // how far short of walls, furniture and ledges the floor stops (m): 0, right up to them
    float gap = 0.25f;          // seams and holes up to this wide are bridged (m)
    float maxSlope = 30;        // steepest floor (degrees): floors are level-ish, walls upright, and what's between
                                // (a cushion's curve, a rock's flank) is neither
    float minArea = 0.5f;       // a separate area smaller than this is dropped (m²)
    float reach = 50;           // how far the area goes from the start, each way (m)
    float metaBounds = 0;       // 1: Meta's invisible boundary walls stop you (Quest homes are made to be seen from a
                                // spot); 0: you walk past them to the rooms and terraces beyond (isMetaBoundary)
};

enum WalkFlag : uint8_t {
    kWalkUp = 1,                // the span's top faces up: something to stand on
    kWalkWall = 2,              // a Wall-role surface is in it: never stood on, blocks at any height
    kWalkFloorRole = 4,         // its top is a Floor-role surface
    kWalkOk = 8,                // walkable (the final area)
    kWalkBridged = 16,          // made by bridging a seam or hole
    kWalkUnreached = 32,        // would be walkable, but can't be reached from the start (or too small)
    kWalkNear = 64,             // a floor of the area closer to its edge than the body radius: where the edge runs
    kWalkFurniture = 128,       // the top of something standing on the floor (a vase, a table, a sofa): never walked on
};

// What a span is made of (WalkSpan::cut; from the bake only, not saved): cut-out geometry — foliage, a hedge, lily
// pads — blocks you, but is never floor, a seat or a backrest.
enum WalkCut : uint8_t {
    kWalkCutTop = 1,            // its top is cut-out geometry
    kWalkCutAll = 2,            // it's all cut-out geometry
};

// dist: a floor's distance to the area's edge in cm (255: that or more; 0: not a floor). walkY: the height you walk at
// there (stairs smoothed into a ramp; worked out from the spans, not saved). cut: saved since FWG5 (a teleport arc
// passes through leaves).
struct WalkSpan { float lo, hi; uint8_t flags, dist; float walkY = 0; uint8_t cut = 0; };

// What a mesh's name says it is: a hint the generator weighs against its shape (src/world_gen.h). Names are often
// whole areas merged ("lounge", "library") or say nothing; then the shape alone decides.
enum WalkHint : uint8_t {
    kHintNone = 0,
    kHintSeat,                  // sofa, chair, bench, stool, pouf, cushions
    kHintWall,                  // a wall, a facade, wall panels
    kHintBlocker,               // a blocker wall: blocks, but no panels on it
    kHintFloor,                 // floor, ground, a rug, stairs
    kHintCeiling,               // ceiling, roof, beams
    kHintFurniture,             // a table, a desk, shelves, a cabinet, a fireplace, a lamp
    kHintNature,                // trees, plants, hedges, planters and pots, rocks
};

// The hint in a mesh's name, past the prefix the home's names share: the last word that
// says something wins ("WithSofa_Wall_Back" is a wall, "deskSeatCushion" a seat), but a planter or a pot is nature
// and a blocker a blocker wherever the word is.
inline WalkHint walkNameHint(const std::string& full, const std::string& prefix = {}) {
    std::vector<std::string> words;
    std::string w;
    const std::string name = full.compare(0, prefix.size(), prefix) == 0 ? full.substr(prefix.size()) : full;
    for (size_t i = 0; i <= name.size(); ++i) {
        const char c = i < name.size() ? name[i] : '_';
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool upper = c >= 'A' && c <= 'Z';
        if (!alpha || (upper && !w.empty() && !(name[i - 1] >= 'A' && name[i - 1] <= 'Z'))) { if (!w.empty()) words.push_back(w); w.clear(); }
        if (alpha) w += static_cast<char>(upper ? c - 'A' + 'a' : c);
    }
    static const std::pair<const char*, WalkHint> kWords[] = {
        {"sofa", kHintSeat}, {"couch", kHintSeat}, {"chair", kHintSeat}, {"chairs", kHintSeat}, {"armchair", kHintSeat},
        {"seat", kHintSeat}, {"seats", kHintSeat}, {"seated", kHintSeat}, {"seating", kHintSeat}, {"bench", kHintSeat},
        {"stool", kHintSeat}, {"stools", kHintSeat}, {"pouf", kHintSeat}, {"ottoman", kHintSeat}, {"cushion", kHintSeat},
        {"cushions", kHintSeat}, {"pillow", kHintSeat}, {"pillows", kHintSeat}, {"loveseat", kHintSeat}, {"lounger", kHintSeat},
        {"chaise", kHintSeat}, {"beanbag", kHintSeat}, {"settee", kHintSeat},
        {"wall", kHintWall}, {"walls", kHintWall}, {"facade", kHintWall}, {"partition", kHintWall}, {"panel", kHintWall}, {"panels", kHintWall},
        {"floor", kHintFloor}, {"ground", kHintFloor}, {"rug", kHintFloor}, {"carpet", kHintFloor}, {"path", kHintFloor},
        {"stairs", kHintFloor}, {"steps", kHintFloor}, {"deck", kHintFloor}, {"terrace", kHintFloor}, {"gravel", kHintFloor}, {"sand", kHintFloor},
        {"ceiling", kHintCeiling}, {"cieling", kHintCeiling}, {"roof", kHintCeiling}, {"beam", kHintCeiling},
        {"table", kHintFurniture}, {"tabletop", kHintFurniture}, {"desk", kHintFurniture}, {"cabinet", kHintFurniture},
        {"shelf", kHintFurniture}, {"shelves", kHintFurniture}, {"bookshelf", kHintFurniture}, {"bookcase", kHintFurniture},
        {"library", kHintFurniture}, {"fireplace", kHintFurniture}, {"counter", kHintFurniture}, {"dresser", kHintFurniture},
        {"lamp", kHintFurniture}, {"lamps", kHintFurniture}, {"sconce", kHintFurniture}, {"vase", kHintFurniture},
        {"sculpture", kHintFurniture}, {"statue", kHintFurniture}, {"centerpiece", kHintFurniture}, {"fan", kHintFurniture},
        {"tree", kHintNature}, {"trees", kHintNature}, {"plant", kHintNature}, {"plants", kHintNature}, {"palm", kHintNature},
        {"foliage", kHintNature}, {"hedge", kHintNature}, {"shrub", kHintNature}, {"bush", kHintNature}, {"leaves", kHintNature},
        {"fern", kHintNature}, {"flower", kHintNature}, {"flowers", kHintNature}, {"branch", kHintNature}, {"rock", kHintNature},
        {"rocks", kHintNature}, {"boulder", kHintNature},
    };
    WalkHint last = kHintNone;
    bool planter = false, blocker = false;
    for (const std::string& t : words) {
        planter |= t == "planter" || t == "pot";
        blocker |= t == "blocker";
        for (const auto& k : kWords) if (t == k.first) last = k.second;
    }
    if (blocker) return kHintBlocker;
    if (planter && last != kHintSeat) return kHintNature;
    return last;
}

// The prefix nearly all of a home's names share ("WithSofa_", "OSE_DELMAR_..._"), up to an underscore: it names the
// home, not the part ("WithSofa_Wall_" is only most of them: the walls).
inline std::string walkSharedPrefix(const std::vector<std::string>& names) {
    std::map<std::string, int> count;
    for (const std::string& n : names)
        for (size_t i = n.find('_'); i != std::string::npos; i = n.find('_', i + 1)) ++count[n.substr(0, i + 1)];
    std::string best;
    for (const auto& kv : count)
        if (names.size() >= 4 && kv.second * 5 >= static_cast<int>(names.size()) * 4 && kv.first.size() > best.size()) best = kv.first;
    return best;
}

// One triangle mesh to rasterize.
struct WalkSource {
    const float* pos = nullptr;     // xyz per vertex, in the source's frame
    size_t verts = 0;
    const uint32_t* idx = nullptr;
    size_t indices = 0;
    float world[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    int role = WorldCollider::Solid;    // Solid, Floor or Wall (Trigger: left out)
    bool noFloor = false;               // blocks but is never stood on (glass and other see-through solids)
    bool cutout = false;                // drawn with cut-out alpha (foliage, a hedge, a screen): solid, but no wall to hang a panel on
    int tag = -1;                       // the caller's id for it (the report's blockers)
    float maxSize = 0;                  // left out when bigger than this across (m; 0: any size)
    bool bounds = true;                 // its extent sets the area's (false: only what falls inside counts: a vista)
    WalkHint hint = kHintNone;          // what its name says it is (the generator's; the bake ignores it)
    std::string name;                   // the mesh's (the generator names its collision after it)
};

struct WalkReport {
    double walkArea = 0, bridgedArea = 0, unreachedArea = 0;   // m²
    int areas = 0, unreached = 0;       // separate walkable areas kept / dropped
    std::vector<std::pair<int, int>> blockers;   // (tag, edge cells it blocks), most first
    int ledgeCells = 0;                 // edge cells at a drop (nothing in the way: a ledge or a hole)
    size_t triangles = 0, spans = 0;
    int nx = 0, nz = 0;
    double ms = 0, msRaster = 0, msBridge = 0;
};

struct WalkGrid {
    WalkParams p;
    float ox = 0, oz = 0;               // the grid's corner (min x, min z)
    int nx = 0, nz = 0;
    std::vector<uint32_t> start;        // per cell (x fastest): first span; start[nx * nz] = spans.size()
    std::vector<WalkSpan> spans;        // per cell, bottom to top
    double walkArea = 0;

    bool cellOf(float x, float z, int& cx, int& cz) const {
        cx = static_cast<int>(std::floor((x - ox) / p.cell));
        cz = static_cast<int>(std::floor((z - oz) / p.cell));
        return cx >= 0 && cz >= 0 && cx < nx && cz < nz;
    }
    bool inside(int cx, int cz) const { return cx >= 0 && cz >= 0 && cx < nx && cz < nz; }
    const WalkSpan* begin(int cx, int cz) const { return spans.data() + start[static_cast<size_t>(cz) * nx + cx]; }
    const WalkSpan* end(int cx, int cz) const { return spans.data() + start[static_cast<size_t>(cz) * nx + cx + 1]; }
    float centerX(int cx) const { return ox + (cx + 0.5f) * p.cell; }
    float centerZ(int cz) const { return oz + (cz + 0.5f) * p.cell; }

    // The span of the area (by default: walkable, or the floor its edge runs over) of a cell nearest height y,
    // within tol (nullptr: none).
    const WalkSpan* walkNear(int cx, int cz, float y, float tol, uint8_t mask = kWalkOk | kWalkNear) const {
        if (!inside(cx, cz)) return nullptr;
        const WalkSpan* best = nullptr;
        float bd = tol;
        for (const WalkSpan* s = begin(cx, cz); s != end(cx, cz); ++s)
            if ((s->flags & mask) && std::fabs(s->hi - y) <= bd) { bd = std::fabs(s->hi - y); best = s; }
        return best;
    }

    // How far inside the area (x, z) is at the level near y, in metres (>= 0: you can stand there): the floors'
    // distances to the edge (what isn't floor just below 0) smoothed over the 3 x 3 cells round it (a quadratic
    // B-spline), less the inset, so the edge is a smooth curve — round furniture is round, a slanted wall straight —
    // that runs up to about the middle of the first cell that isn't floor: where on average the wall or the drop is.
    // h: the floor height there (ramps exact); g: the gradient (points into the area, about unit length at an edge).
    float field(float x, float z, float y, float* h = nullptr, float* g = nullptr) const {
        const float fx = (x - ox) / p.cell - 0.5f, fz = (z - oz) / p.cell - 0.5f;
        const int ic = static_cast<int>(std::lround(fx)), jc = static_cast<int>(std::lround(fz));
        const float tx = fx - ic, tz = fz - jc;   // -0.5 .. 0.5 from the nearest centre
        const float wx[3] = {0.5f * (0.5f - tx) * (0.5f - tx), 0.75f - tx * tx, 0.5f * (0.5f + tx) * (0.5f + tx)};
        const float wz[3] = {0.5f * (0.5f - tz) * (0.5f - tz), 0.75f - tz * tz, 0.5f * (0.5f + tz) * (0.5f + tz)};
        const float dx[3] = {tx - 0.5f, -2 * tx, tx + 0.5f}, dz[3] = {tz - 0.5f, -2 * tz, tz + 0.5f};   // d/dt of the weights
        constexpr float kOut = -0.02f;            // not floor, with no floor by it to go on from: just below the edge
        float f = -p.inset, gx = 0, gz = 0;
        const WalkSpan* c[9];
        auto distAt = [&](int i, int j, float& v) { const WalkSpan* q = walkNear(i, j, y, p.step); if (q) v = q->dist * 0.01f; return q != nullptr; };
        // A cell that isn't floor, beside one that is: the floor's distance to the edge carried on across it (2a - b
        // from the two floor cells in a row towards it), so the edge falls where the distances say — on the straight
        // line along a wall at an angle, not on the cells' staircase.
        auto outside = [&](int ci, int cj) {
            static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
            float sum = 0;
            int n = 0;
            for (const auto& d : dirs) {
                float a, b;
                if (!distAt(ci + d[0], cj + d[1], a)) continue;
                const float stepLen = d[0] && d[1] ? p.cell * 1.41421f : p.cell;
                const float est = distAt(ci + 2 * d[0], cj + 2 * d[1], b) && b > a + 1e-4f ? 2 * a - b : a - stepLen;
                sum += std::min(est, 0.5f * p.cell);
                ++n;
            }
            return n ? sum / n : kOut;
        };
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                const WalkSpan* s = c[j * 3 + i] = walkNear(ic - 1 + i, jc - 1 + j, y, p.step);
                const float v = s ? s->dist * 0.01f : outside(ic - 1 + i, jc - 1 + j);
                f += wx[i] * wz[j] * v;
                gx += dx[i] * wz[j] * v;
                gz += wx[i] * dz[j] * v;
            }
        if (g) { g[0] = gx / p.cell; g[1] = gz / p.cell; }
        if (h) {
            // the height: bilinear between the 4 centres round (x, z), from the corners on the same surface as the
            // nearest one (a step stays a step)
            const int i0 = static_cast<int>(std::floor(fx)), j0 = static_cast<int>(std::floor(fz));
            const float bx = fx - i0, bz = fz - j0;
            const float w[4] = {(1 - bx) * (1 - bz), bx * (1 - bz), (1 - bx) * bz, bx * bz};
            const WalkSpan* q[4];
            int nearest = -1;
            float nw = -1;
            for (int k = 0; k < 4; ++k) {
                q[k] = walkNear(i0 + (k & 1), j0 + (k >> 1), y, p.step);
                if (q[k] && w[k] > nw) { nw = w[k]; nearest = k; }
            }
            if (nearest >= 0) {
                const float tight = p.cell * 1.2f + 0.02f;   // a 50 degree ramp still blends
                float sw = 0, sh = 0;
                for (int k = 0; k < 4; ++k)
                    if (q[k] && std::fabs(q[k]->walkY - q[nearest]->walkY) <= tight) { sw += w[k] + 1e-6f; sh += (w[k] + 1e-6f) * q[k]->walkY; }
                *h = sh / sw;
            } else *h = y;
        }
        (void)c;
        return f;
    }

    bool walkable(float x, float z, float y, float* h = nullptr) const { return field(x, z, y, h) >= 0; }

    // Highest floor under (x, z) between y - maxDown and y + maxUp. Walkable floor first; else (standing somewhere
    // the area doesn't reach, roomscale: by a wall, under a table) anything to stand on at most 10 cm up.
    bool floorAt(float x, float z, float y, float maxUp, float maxDown, float& out) const {
        // the levels of the floors round it (the 3 x 3 cells field() reads: the edge runs past the last floor cell)
        const int ic = static_cast<int>(std::lround((x - ox) / p.cell - 0.5f)), jc = static_cast<int>(std::lround((z - oz) / p.cell - 0.5f));
        float lv[24];
        int n = 0;
        for (int k = 0; k < 9; ++k) {
            const int cx = ic - 1 + k % 3, cz = jc - 1 + k / 3;
            if (!inside(cx, cz)) continue;
            for (const WalkSpan* s = begin(cx, cz); s != end(cx, cz) && n < 24; ++s)
                if ((s->flags & (kWalkOk | kWalkNear)) && s->hi <= y + maxUp && s->hi >= y - maxDown) lv[n++] = s->hi;
        }
        std::sort(lv, lv + n, [](float a, float b) { return a > b; });
        for (int k = 0; k < n; ++k) {
            float h;
            if (field(x, z, lv[k], &h) >= 0 && h <= y + maxUp + 0.02f && h >= y - maxDown - 0.02f) { out = h; return true; }
        }
        int cx, cz;
        if (!cellOf(x, z, cx, cz)) return false;
        bool found = false;
        for (const WalkSpan* s = begin(cx, cz); s != end(cx, cz); ++s)
            if ((s->flags & (kWalkUp | kWalkOk | kWalkUnreached)) && !(s->flags & kWalkWall) && s->hi <= y + std::min(maxUp, 0.1f) && s->hi >= y - maxDown) {
                out = s->hi;
                found = true;
            }
        return found;
    }

    // Walks a standing body at height y from (x, z) by (dx, dz) through the area, sliding along its edges; never
    // off it, up a ledge or down one. Starting outside it (roomscale took you there), any move that doesn't go
    // further out is allowed, so you can always walk back in.
    void move(float& x, float& z, float y, float dx, float dz) const {
        float yRef = y, h;
        float f0 = field(x, z, y, &h);
        if (f0 >= 0) yRef = h;
        const float len = std::sqrt(dx * dx + dz * dz);
        if (len < 1e-7f) return;
        const int n = std::max(1, static_cast<int>(std::ceil(len / (p.cell * 0.4f))));
        const float sx = dx / n, sz = dz / n;
        auto ok = [&](float qx, float qz, float& fq, float& hq) {
            fq = field(qx, qz, yRef, &hq);
            if (fq >= 0) return true;
            if (f0 >= 0) return false;
            float f;
            return fq >= f0 - 1e-4f && floorAt(qx, qz, yRef, p.step, p.step, f) && ((hq = f), true);
        };
        for (int i = 0; i < n; ++i) {
            float fq, hq;
            if (ok(x + sx, z + sz, fq, hq)) { x += sx; z += sz; yRef = hq; f0 = std::max(f0, fq); continue; }
            // slide: from inside, take the step and push it back in along the field's gradient (the edge is a
            // distance, so this lands on it: a smooth glide along curves and slanted walls, no zig-zag)
            if (f0 >= 0) {
                float qx = x + sx, qz = z + sz, f = fq, g[2];
                for (int it = 0; it < 3 && f < 0; ++it) {
                    f = field(qx, qz, yRef, nullptr, g);
                    const float g2 = g[0] * g[0] + g[1] * g[1];
                    if (f >= 0 || g2 < 1e-6f) break;
                    const float k = (1e-4f - f) / g2;
                    qx += g[0] * k; qz += g[1] * k;
                }
                const float mx = qx - x, mz = qz - z;
                if (mx * sx + mz * sz > 0 && mx * mx + mz * mz <= (sx * sx + sz * sz) * 1.0201f && ok(qx, qz, fq, hq)) {
                    x = qx; z = qz; yRef = hq; f0 = std::max(f0, fq);
                    continue;
                }
            }
            // else drop the part of the step that goes out of the area (along the field's gradient)
            float g[2];
            field(x + sx, z + sz, yRef, nullptr, g);
            float gl = std::sqrt(g[0] * g[0] + g[1] * g[1]);
            if (gl < 1e-6f) { field(x, z, yRef, nullptr, g); gl = std::sqrt(g[0] * g[0] + g[1] * g[1]); }
            bool moved = false;
            if (gl > 1e-6f) {
                const float nx = g[0] / gl, nz = g[1] / gl, into = sx * nx + sz * nz;
                if (into < 0) {
                    const float tx = sx - into * nx, tz = sz - into * nz;
                    if (ok(x + tx, z + tz, fq, hq)) { x += tx; z += tz; yRef = hq; f0 = std::max(f0, fq); moved = true; }
                }
            }
            if (!moved) {
                if (ok(x + sx, z, fq, hq)) { x += sx; yRef = hq; f0 = std::max(f0, fq); moved = true; }
                else if (ok(x, z + sz, fq, hq)) { z += sz; yRef = hq; f0 = std::max(f0, fq); moved = true; }
            }
            if (!moved) return;
        }
    }

    // Somewhere to stand near (x, y, z): there, or the nearest walkable spot within `nudge` on the same level.
    bool roomAt(float& x, float y, float& z, float nudge = 0.3f) const {
        if (walkable(x, z, y)) return true;
        int cx, cz;
        cellOf(x, z, cx, cz);
        const int r = static_cast<int>(std::ceil(nudge / p.cell));
        float best = nudge * nudge, bx = 0, bz = 0;
        bool found = false;
        for (int j = cz - r; j <= cz + r; ++j)
            for (int i = cx - r; i <= cx + r; ++i) {
                if (!walkNear(i, j, y, p.step)) continue;
                const float px = centerX(i), pz = centerZ(j), d = (px - x) * (px - x) + (pz - z) * (pz - z);
                if (d < best && walkable(px, pz, y)) { best = d; bx = px; bz = pz; found = true; }
            }
        if (found) { x = bx; z = bz; }
        return found;
    }

    // A head (the eyes at (x, y, z), a ball of radius r) with nothing solid in it: how far to move it over XZ, to the
    // nearest place where nothing solid at its height (a wall, a shelf, a lamp, a table top bent over; not the floors
    // you stand on) is closer than r. False: it's clear already (or there's no clear place within 0.6 m).
    bool headPush(float x, float y, float z, float r, float push[2]) const {
        push[0] = push[1] = 0;
        const int n = static_cast<int>(std::ceil((r + 0.6f) / p.cell));
        int cx, cz;
        cellOf(x, z, cx, cz);
        std::vector<std::array<float, 4>> solid;                       // the solid cells' squares round it
        for (int j = cz - n; j <= cz + n; ++j)
            for (int i = cx - n; i <= cx + n; ++i) {
                if (!inside(i, j)) continue;
                bool hit = false;
                for (const WalkSpan* s = begin(i, j); s != end(i, j) && !hit; ++s)
                    hit = !(s->flags & (kWalkOk | kWalkNear)) && s->hi >= y - r && s->lo <= y + 0.1f;
                if (hit) solid.push_back({ox + i * p.cell, oz + j * p.cell, ox + (i + 1) * p.cell, oz + (j + 1) * p.cell});
            }
        auto clear = [&](float qx, float qz) {
            for (const auto& b : solid) {
                const float dx = qx - std::clamp(qx, b[0], b[2]), dz = qz - std::clamp(qz, b[1], b[3]);
                if (dx * dx + dz * dz < r * r) return false;
            }
            return true;
        };
        if (clear(x, z)) return false;
        // the nearest clear place: rings outward, 2.5 cm apart
        for (int ring = 1; ring <= 24; ++ring) {
            const float d = ring * 0.025f;
            const int k = std::max(8, static_cast<int>(d * 6.2832f / 0.025f));
            float best = 1e30f;
            for (int a = 0; a < k; ++a) {
                const float t = a * 6.2832f / k, qx = x + d * std::cos(t), qz = z + d * std::sin(t);
                if (!clear(qx, qz)) continue;
                const float w = (qx - x) * (qx - x) + (qz - z) * (qz - z);
                if (w < best) { best = w; push[0] = qx - x; push[1] = qz - z; }
            }
            if (best < 1e29f) return true;
        }
        return false;
    }

    // A body standing at (x, z) on the floor near y that isn't in the area because something is there (roomscale:
    // you walked into a coffee table for real; not a drop: that you fall off): how far to move it back to the area's
    // edge, the nearest way. False: it's in the area, over a drop, or no floor is within 0.6 m.
    bool bodyPush(float x, float y, float z, float push[2]) const {
        push[0] = push[1] = 0;
        if (field(x, z, y) >= 0) return false;
        int cx, cz;
        if (!cellOf(x, z, cx, cz)) return false;
        bool blocked = false;
        for (const WalkSpan* s = begin(cx, cz); s != end(cx, cz) && !blocked; ++s) blocked = s->hi > y + 0.15f && s->lo < y + 1.2f;
        if (!blocked) return false;
        float tx = x, tz = z;
        if (!roomAt(tx, y, tz, 0.6f)) return false;
        float a = 0, b = 1;                                            // the edge between here and there
        for (int i = 0; i < 8; ++i) { const float m = (a + b) * 0.5f; (field(x + (tx - x) * m, z + (tz - z) * m, y) >= 0 ? b : a) = m; }
        push[0] = (tx - x) * b; push[1] = (tz - z) * b;
        return true;
    }

    // What a straight segment o + d t, t in [0, 1], meets first: 1 = the top of something to stand on (Land: the
    // caller decides if there's room), 2 = anything else solid (Block), 0 = nothing. n: the surface's normal facing
    // the segment.
    int trace(const float o[3], const float d[3], float& tOut, float n[3]) const {
        int cx, cz;
        cellOf(o[0], o[2], cx, cz);
        const int sx = d[0] > 0 ? 1 : -1, sz = d[2] > 0 ? 1 : -1;
        const float ix = std::fabs(d[0]) > 1e-9f ? p.cell / std::fabs(d[0]) : 1e30f, iz = std::fabs(d[2]) > 1e-9f ? p.cell / std::fabs(d[2]) : 1e30f;
        float tx = std::fabs(d[0]) > 1e-9f ? (ox + (sx > 0 ? cx + 1 : cx) * p.cell - o[0]) / d[0] : 1e30f;
        float tz = std::fabs(d[2]) > 1e-9f ? (oz + (sz > 0 ? cz + 1 : cz) * p.cell - o[2]) / d[2] : 1e30f;
        float ta = 0;
        int axis = -1;                                                 // the axis crossed into this cell
        for (int guard = 0; guard < 100000; ++guard) {
            const float tb = std::min({tx, tz, 1.0f});
            if (inside(cx, cz)) {
                const float ya = o[1] + d[1] * ta, yb = o[1] + d[1] * tb;
                const float ymin = std::min(ya, yb), ymax = std::max(ya, yb);
                float best = 2;
                int kind = 0;
                float bn[3] = {0, 1, 0};
                for (const WalkSpan* s = begin(cx, cz); s != end(cx, cz); ++s) {
                    if (s->hi < ymin || s->lo > ymax) continue;
                    if ((s->cut & kWalkCutAll) && !(s->flags & (kWalkOk | kWalkNear))) continue;   // leaves: the arc goes through
                    const bool stand = (s->flags & (kWalkUp | kWalkOk | kWalkUnreached)) && !(s->flags & kWalkWall);
                    float t;
                    int k;
                    float sn[3] = {0, 0, 0};
                    if (ya > s->hi) { t = (s->hi - o[1]) / d[1]; k = stand ? 1 : 2; sn[1] = 1; }
                    else if (ya < s->lo) { t = (s->lo - o[1]) / d[1]; k = 2; sn[1] = -1; }
                    else {
                        if (axis < 0) continue;                        // starting inside it
                        t = ta;
                        if (stand && s->hi - ya <= 0.1f) { k = 1; sn[1] = 1; }
                        else { k = 2; if (axis == 0) sn[0] = -static_cast<float>(sx); else sn[2] = -static_cast<float>(sz); }
                    }
                    if (t < best) { best = t; kind = k; std::copy_n(sn, 3, bn); }
                }
                if (kind) {
                    tOut = std::clamp(best, 0.0f, 1.0f);
                    if (kind == 1) slopeNormal(o[0] + d[0] * tOut, o[2] + d[2] * tOut, o[1] + d[1] * tOut, bn);
                    std::copy_n(bn, 3, n);
                    return kind;
                }
            }
            if (tb >= 1.0f) break;
            ta = tb;
            if (tx < tz) { tx += ix; cx += sx; axis = 0; } else { tz += iz; cz += sz; axis = 1; }
        }
        tOut = 2;
        return 0;
    }

    bool any() const { return walkArea > 0; }

    // walkY for every floor of the area: its height averaged with its neighbours' on the same walk (within a step), two
    // passes of 3 x 3: a flight of stairs becomes a ramp you glide up, a flat floor stays exactly where it is.
    void smoothHeights() {
        for (WalkSpan& s : spans) s.walkY = s.hi;
        std::vector<float> next(spans.size());
        for (int pass = 0; pass < 2; ++pass) {
            for (int z = 0; z < nz; ++z)
                for (int x = 0; x < nx; ++x)
                    for (const WalkSpan* s = begin(x, z); s != end(x, z); ++s) {
                        const size_t i = s - spans.data();
                        next[i] = s->walkY;
                        if (!(s->flags & (kWalkOk | kWalkNear))) continue;
                        float sum = 0, n = 0;
                        for (int j = -1; j <= 1; ++j)
                            for (int k = -1; k <= 1; ++k)
                                if (const WalkSpan* q = walkNear(x + k, z + j, s->hi, p.step)) { sum += q->walkY; n += 1; }
                        if (n > 0) next[i] = sum / n;
                    }
            for (size_t i = 0; i < spans.size(); ++i) spans[i].walkY = next[i];
        }
    }

    // The area as it's walked (field() >= 0), for drawing, per level: between cell centres well inside it, the quad;
    // where the edge runs, marching squares on field() sampled every quarter cell, so the drawn edge is the smooth
    // one you walk to. tri(a, b, c, bridged): a triangle of floor; edge(a, b): a piece of its edge. Points are xyz on
    // the floor (the treads' own heights).
    template <typename Tri, typename Edge> void outline(Tri&& tri, Edge&& edge) const {
        static const int kx[4] = {0, 1, 1, 0}, kz[4] = {0, 0, 1, 1};   // the quad's corners, anticlockwise from above
        constexpr int N = 4;                                            // samples per cell along an edge
        // Flat quads well inside the area are merged along x into one quad per run (a level floor is most of any
        // area: ~10x fewer triangles to draw). A run stays open while the next cell continues it at the same height.
        struct FlatRun { int x0, x1; float h; bool bridged; };
        std::vector<FlatRun> runs;
        auto drawRuns = [&](int z) {
            const float za = centerZ(z), zb = za + p.cell;
            for (const FlatRun& r : runs) {
                const float xa = centerX(r.x0), xb = centerX(r.x1) + p.cell;
                const float q0[3] = {xa, r.h, za}, q1[3] = {xb, r.h, za}, q2[3] = {xb, r.h, zb}, q3[3] = {xa, r.h, zb};
                tri(q0, q1, q2, r.bridged);
                tri(q0, q2, q3, r.bridged);
            }
            runs.clear();
        };
        for (int z = 0; z + 1 < nz; drawRuns(z), ++z)
            for (int x = 0; x + 1 < nx; ++x)
                for (int k0 = 0; k0 < 4; ++k0)
                    for (const WalkSpan* s0 = begin(x + kx[k0], z + kz[k0]); s0 != end(x + kx[k0], z + kz[k0]); ++s0) {
                        if (!(s0->flags & (kWalkOk | kWalkNear))) continue;
                        bool earlier = false;                      // this level is drawn from an earlier corner
                        for (int j = 0; j < k0 && !earlier; ++j) earlier = walkNear(x + kx[j], z + kz[j], s0->hi, p.step) != nullptr;
                        if (earlier) continue;
                        float h[4];
                        bool bridged = false, all = true;
                        int deep = 0;
                        for (int k = 0; k < 4; ++k) {
                            const WalkSpan* c = walkNear(x + kx[k], z + kz[k], s0->hi, p.step);
                            h[k] = c ? c->hi : s0->hi;
                            all &= c != nullptr;
                            deep += c && c->dist * 0.01f >= p.inset + 1.5f * p.cell;
                            bridged |= c && (c->flags & kWalkBridged);
                        }
                        const float x0 = centerX(x), z0 = centerZ(z), cs = p.cell;
                        auto at = [&](float u, float v, float* o) {   // (u, v) in 0..1 over the quad, on the floor
                            o[0] = x0 + u * cs;
                            o[2] = z0 + v * cs;
                            o[1] = (h[0] * (1 - u) + h[1] * u) * (1 - v) + (h[3] * (1 - u) + h[2] * u) * v;
                        };
                        if (all && deep == 4 && h[0] == h[1] && h[1] == h[2] && h[2] == h[3]) {   // well inside and level
                            bool extended = false;
                            for (FlatRun& r : runs)
                                if (r.x1 == x - 1 && r.h == h[0] && r.bridged == bridged) { r.x1 = x; extended = true; break; }
                            if (!extended) runs.push_back({x, x, h[0], bridged});
                            continue;
                        }
                        if (all && deep == 4) {                    // well inside: the whole quad
                            float q[4][3];
                            for (int k = 0; k < 4; ++k) at(static_cast<float>(kx[k]), static_cast<float>(kz[k]), q[k]);
                            tri(q[0], q[1], q[2], bridged);
                            tri(q[0], q[2], q[3], bridged);
                            continue;
                        }
                        float f[N + 1][N + 1];
                        bool any = false;
                        for (int j = 0; j <= N; ++j)
                            for (int i = 0; i <= N; ++i) {
                                f[j][i] = field(x0 + cs * i / N, z0 + cs * j / N, s0->hi);
                                any |= f[j][i] >= 0;
                            }
                        if (!any) continue;
                        for (int j = 0; j < N; ++j)
                            for (int i = 0; i < N; ++i) {
                                const int si[4] = {i, i + 1, i + 1, i}, sj[4] = {j, j, j + 1, j + 1};
                                float v[4];
                                for (int k = 0; k < 4; ++k) v[k] = f[sj[k]][si[k]];
                                float poly[8][3];
                                bool leaves[8] = {}, cut[8] = {};
                                int n = 0;
                                for (int k = 0; k < 4; ++k) {
                                    const int k1 = (k + 1) & 3;
                                    const float u = static_cast<float>(si[k]) / N, w = static_cast<float>(sj[k]) / N;
                                    if (v[k] >= 0) { at(u, w, poly[n]); ++n; }
                                    if ((v[k] >= 0) != (v[k1] >= 0)) {
                                        const float t = v[k] / (v[k] - v[k1]);
                                        const float u1 = static_cast<float>(si[k1]) / N, w1 = static_cast<float>(sj[k1]) / N;
                                        at(u + (u1 - u) * t, w + (w1 - w) * t, poly[n]);
                                        cut[n] = true;
                                        leaves[n] = v[k] >= 0;
                                        ++n;
                                    }
                                }
                                for (int q = 1; q + 1 < n; ++q) tri(poly[0], poly[q], poly[q + 1], bridged);
                                // the edge: from where the polygon leaves the area to where it comes back in
                                for (int q = 0; q < n; ++q)
                                    if (cut[q] && leaves[q] && cut[(q + 1) % n]) edge(poly[q], poly[(q + 1) % n]);
                            }
                    }
    }

private:
    void slopeNormal(float x, float z, float y, float n[3]) const {
        float h0, hx, hz;
        if (field(x, z, y + 0.05f, &h0) < 0 || field(x + p.cell, z, h0, &hx) < 0 || field(x, z + p.cell, h0, &hz) < 0) { n[0] = n[2] = 0; n[1] = 1; return; }
        const float a = -(hx - h0) / p.cell, b = -(hz - h0) / p.cell, L = std::sqrt(a * a + 1 + b * b);
        n[0] = a / L; n[1] = 1 / L; n[2] = b / L;
    }
};

// ── Building ────────────────────────────────────────────────────────────────────────────────────

namespace walkgrid_detail {

struct BSpan { float lo, hi; int next; int tag; uint8_t flags, cut; };

struct Raster {
    float ox, oz, cs;                   // the whole grid's corner: rows are the grid's (a band's are the same numbers)
    int nx, nz;                         // nz: this band's rows, from row z0
    int z0 = 0;
    std::vector<int> head;
    std::vector<BSpan> pool;
    int freeList = -1;
    static constexpr float kMergeGap = 0.1f;   // spans closer than this are one (a rug on the floor, a slab's faces)
    static constexpr float kTopEps = 0.05f;    // tops this close share what they are (a lip on a floor stays floor)

    int alloc(const BSpan& s) {
        if (freeList >= 0) { const int i = freeList; freeList = pool[i].next; pool[i] = s; return i; }
        pool.push_back(s);
        return static_cast<int>(pool.size()) - 1;
    }
    void add(int cell, float lo, float hi, uint8_t flags, int tag, uint8_t cut = 0) {
        BSpan s{lo, hi, -1, tag, flags, cut};
        int prev = -1, cur = head[cell];
        while (cur >= 0) {
            BSpan& c = pool[cur];
            if (c.lo > s.hi + kMergeGap) break;
            if (c.hi < s.lo - kMergeGap) { prev = cur; cur = c.next; continue; }
            constexpr uint8_t top = kWalkUp | kWalkFloorRole | kWalkFurniture;
            const bool sCut = s.cut & kWalkCutAll, cCut = c.cut & kWalkCutAll;
            if (sCut != cCut && (sCut ? c.hi >= s.hi - 0.15f : s.hi >= c.hi - 0.15f)) {
                // cut-out geometry lying on something solid (grass on the ground, petals on a path): the solid top decides
                if (sCut) { s.flags = static_cast<uint8_t>((s.flags & ~top) | (c.flags & top)); s.tag = c.tag; }
                s.cut = 0;
            } else {
                if (c.hi > s.hi + kTopEps) { s.flags = static_cast<uint8_t>((s.flags & ~top) | (c.flags & top)); s.tag = c.tag; }
                else if (std::fabs(c.hi - s.hi) <= kTopEps) { s.flags |= c.flags & top; if (c.hi > s.hi) s.tag = c.tag; }
                const uint8_t cutTop = c.hi > s.hi + kTopEps ? c.cut & kWalkCutTop : std::fabs(c.hi - s.hi) <= kTopEps ? s.cut & c.cut & kWalkCutTop : s.cut & kWalkCutTop;
                s.cut = static_cast<uint8_t>(cutTop | (s.cut & c.cut & kWalkCutAll));
            }
            s.flags |= c.flags & kWalkWall;
            s.lo = std::min(s.lo, c.lo);
            s.hi = std::max(s.hi, c.hi);
            const int nxt = c.next;
            c.next = freeList; freeList = cur;
            if (prev < 0) head[cell] = nxt; else pool[prev].next = nxt;
            cur = nxt;
        }
        if (flags & kWalkBridged) {                                    // a bridge: the top here is something to stand on
            s.flags |= kWalkUp | kWalkBridged;                         // (a riser's bevel between two floors, a chamfer)
            s.cut = 0;
        }
        const int i = alloc(s);
        pool[i].next = cur;
        if (prev < 0) head[cell] = i; else pool[prev].next = i;
    }

    // Sutherland-Hodgman against one side of an axis-aligned plane (keep: v[axis] * sign <= c * sign + eps).
    static int clip(const float* in, int n, float* out, int axis, float c, float sign) {
        int m = 0;
        constexpr float eps = 1e-4f;
        for (int i = 0; i < n; ++i) {
            const float* a = in + i * 3;
            const float* b = in + ((i + 1) % n) * 3;
            const float da = (a[axis] - c) * sign - eps, db = (b[axis] - c) * sign - eps;
            if (da <= 0) { std::copy_n(a, 3, out + m * 3); ++m; }
            if ((da <= 0) != (db <= 0)) {
                const float t = da / (da - db);
                for (int k = 0; k < 3; ++k) out[m * 3 + k] = a[k] + (b[k] - a[k]) * t;
                ++m;
            }
        }
        return m;
    }

    void triangle(const float* a, const float* b, const float* c, uint8_t flags, int tag, uint8_t cut = 0) {
        const float minx = std::min({a[0], b[0], c[0]}), maxx = std::max({a[0], b[0], c[0]});
        const float minz = std::min({a[2], b[2], c[2]}), maxz = std::max({a[2], b[2], c[2]});
        const int rx0 = static_cast<int>(std::floor((minx - ox) / cs)), rx1 = static_cast<int>(std::floor((maxx - ox) / cs));
        const int rz0 = static_cast<int>(std::floor((minz - oz) / cs)), rz1 = static_cast<int>(std::floor((maxz - oz) / cs));
        const int x0 = std::max(0, rx0), x1 = std::min(nx - 1, rx1), z0 = std::max(this->z0, rz0), z1 = std::min(this->z0 + nz - 1, rz1);
        if (x0 > x1 || z0 > z1) return;
        const int zb = this->z0;
        if (rx0 == rx1 && rz0 == rz1) {                                // inside one column (most of a dense mesh); one cut
                                                                       // off by the edge (of a band) is clipped below
            add((z0 - zb) * nx + x0, std::min({a[1], b[1], c[1]}), std::max({a[1], b[1], c[1]}), flags, tag, cut);
            return;
        }
        float tri[9], row[3 * 12], cellp[3 * 12], tmp[3 * 12];
        std::copy_n(a, 3, tri); std::copy_n(b, 3, tri + 3); std::copy_n(c, 3, tri + 6);
        // A cell wholly inside the triangle (seen from above) needs no clipping: its heights are the triangle's plane
        // at the cell's corners. Only the cells on the triangle's outline are clipped (a big floor is mostly inside).
        const float area2 = (b[0] - a[0]) * (c[2] - a[2]) - (c[0] - a[0]) * (b[2] - a[2]);
        const bool planar = std::fabs(area2) > 1e-9f;
        float ea[3][3] = {}, py[3] = {};                               // edge functions (inside >= 0) and y = py . (x, z, 1)
        if (planar) {
            const float sg = area2 > 0 ? 1.0f : -1.0f;
            const float* v[3] = {a, b, c};
            for (int e = 0; e < 3; ++e) {
                const float* p0 = v[e]; const float* p1 = v[(e + 1) % 3];
                ea[e][0] = -(p1[2] - p0[2]) * sg; ea[e][1] = (p1[0] - p0[0]) * sg; ea[e][2] = -(ea[e][0] * p0[0] + ea[e][1] * p0[2]);
            }
            py[0] = ((b[1] - a[1]) * (c[2] - a[2]) - (c[1] - a[1]) * (b[2] - a[2])) / area2;
            py[1] = ((c[1] - a[1]) * (b[0] - a[0]) - (b[1] - a[1]) * (c[0] - a[0])) / area2;
            py[2] = a[1] - py[0] * a[0] - py[1] * a[2];
        }
        auto inside = [&](float x, float z) {
            for (int e = 0; e < 3; ++e) if (ea[e][0] * x + ea[e][1] * z + ea[e][2] < 0) return false;
            return true;
        };
        for (int z = z0; z <= z1; ++z) {
            const float zl = oz + z * cs, zh = zl + cs;
            int nr = clip(tri, 3, tmp, 2, zl, -1.0f);
            if (nr < 3) continue;
            nr = clip(tmp, nr, row, 2, zh, 1.0f);
            if (nr < 3) continue;
            float rminx = 1e30f, rmaxx = -1e30f;
            for (int i = 0; i < nr; ++i) { rminx = std::min(rminx, row[i * 3]); rmaxx = std::max(rmaxx, row[i * 3]); }
            const int rx0 = std::max(x0, static_cast<int>(std::floor((rminx - ox) / cs))), rx1 = std::min(x1, static_cast<int>(std::floor((rmaxx - ox) / cs)));
            for (int x = rx0; x <= rx1; ++x) {
                const float xl = ox + x * cs, xh = xl + cs;
                if (planar && inside(xl, zl) && inside(xh, zl) && inside(xl, zh) && inside(xh, zh)) {
                    const float y00 = py[0] * xl + py[1] * zl + py[2], y10 = y00 + py[0] * cs, y01 = y00 + py[1] * cs, y11 = y10 + py[1] * cs;
                    add((z - zb) * nx + x, std::min({y00, y10, y01, y11}), std::max({y00, y10, y01, y11}), flags, tag, cut);
                    continue;
                }
                int nc = clip(row, nr, tmp, 0, xl, -1.0f);
                if (nc < 3) continue;
                nc = clip(tmp, nc, cellp, 0, xh, 1.0f);
                if (nc < 3) continue;
                float lo = 1e30f, hi = -1e30f;
                for (int i = 0; i < nc; ++i) { lo = std::min(lo, cellp[i * 3 + 1]); hi = std::max(hi, cellp[i * 3 + 1]); }
                add((z - zb) * nx + x, lo, hi, flags, tag, cut);
            }
        }
    }
};

inline void xform(const float* m, const float* p, float* o) {
    for (int k = 0; k < 3; ++k) o[k] = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k];
}

// The grid's rows go in bands, several per core (a home's geometry is anything but even), handed out to up to 8
// threads as they finish: fn(z0, z1, band) for each band, which only writes its own rows.
inline int walkThreads() { return static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 8u)); }
inline int bandCount(int rows) { return std::clamp(rows / 16, 1, walkThreads() * 4); }
template <typename F> void parallelBands(int rows, int bands, F&& fn) {
    std::atomic<int> next{0};
    auto work = [&] { for (int b; (b = next++) < bands;) fn(rows * b / bands, rows * (b + 1) / bands, b); };
    const int extra = std::min(walkThreads(), bands) - 1;
    std::vector<std::thread> th;
    th.reserve(extra);
    for (int i = 0; i < extra; ++i) th.emplace_back(work);
    work();
    for (std::thread& t : th) t.join();
}

}  // namespace walkgrid_detail

// A box / sphere / capsule collider as world-space triangles (for WalkSource).
inline void walkShapeMesh(const WorldCollider& c, std::vector<float>& pos, std::vector<uint32_t>& idx) {
    pos.clear(); idx.clear();
    auto put = [&](float x, float y, float z) {
        float l[3] = {x, y, z}, o[3];
        walkgrid_detail::xform(c.world, l, o);
        pos.insert(pos.end(), o, o + 3);
    };
    if (c.kind == WorldCollider::Box) {
        for (int k = 0; k < 8; ++k) put((k & 1 ? 1 : -1) * c.half[0], (k & 2 ? 1 : -1) * c.half[1], (k & 4 ? 1 : -1) * c.half[2]);
        static const uint32_t f[36] = {0, 1, 3, 0, 3, 2, 4, 6, 7, 4, 7, 5, 0, 4, 5, 0, 5, 1, 2, 3, 7, 2, 7, 6, 0, 2, 6, 0, 6, 4, 1, 5, 7, 1, 7, 3};
        idx.assign(f, f + 36);
        return;
    }
    if (c.kind != WorldCollider::Sphere && c.kind != WorldCollider::Capsule) return;
    const float hl = c.kind == WorldCollider::Capsule ? c.length * 0.5f : 0.0f, r = c.radius;
    const int seg = 12, rings = 8;                                    // a UV sphere, split at the equator for capsules
    for (int i = 0; i <= rings; ++i) {
        const float th = 3.14159265f * i / rings, y = std::cos(th) * r + (i <= rings / 2 ? hl : -hl), rr = std::sin(th) * r;
        for (int j = 0; j < seg; ++j) { const float ph = 6.2831853f * j / seg; put(rr * std::cos(ph), y, rr * std::sin(ph)); }
    }
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < seg; ++j) {
            const uint32_t a = i * seg + j, b = i * seg + (j + 1) % seg, cc = a + seg, d = b + seg;
            idx.insert(idx.end(), {a, cc, b, b, cc, d});
        }
}

// Bakes the walkable area. seeds: where the player starts or is taken (spawns, hotspots, seat exits); the areas
// they're on are kept (none: the one at `anchor`, else the biggest). anchor: the middle of the home (the reach is
// measured from it; default the first seed, else the origin).
inline std::shared_ptr<WalkGrid> buildWalkGrid(const std::vector<WalkSource>& sources, const std::vector<std::array<float, 3>>& seeds,
                                               const WalkParams& P, WalkReport* report = nullptr, const float* anchorIn = nullptr) {
    using namespace walkgrid_detail;
    const auto t0 = std::chrono::steady_clock::now();
    WalkReport rep;
    auto g = std::make_shared<WalkGrid>();
    g->p = P;
    const float cs = P.cell;
    float anchor[3] = {0, 0, 0};
    if (anchorIn) std::copy_n(anchorIn, 3, anchor);
    else if (!seeds.empty()) std::copy_n(seeds[0].data(), 3, anchor);

    // World-space vertices of everything, and the area they cover near the anchor.
    std::vector<std::vector<float>> wpos(sources.size());
    std::vector<std::array<float, 4>> box(sources.size());          // XZ extent of each
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    for (size_t s = 0; s < sources.size(); ++s) {
        const WalkSource& src = sources[s];
        if (src.role == WorldCollider::Trigger || !src.pos || !src.idx) continue;
        std::vector<float>& wp = wpos[s];
        wp.resize(src.verts * 3);
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t v = 0; v < src.verts; ++v) {
            xform(src.world, src.pos + v * 3, &wp[v * 3]);
            for (int k = 0; k < 3; ++k) { mn[k] = std::min(mn[k], wp[v * 3 + k]); mx[k] = std::max(mx[k], wp[v * 3 + k]); }
        }
        if (src.maxSize > 0 && (mx[0] - mn[0] > src.maxSize || mx[1] - mn[1] > src.maxSize || mx[2] - mn[2] > src.maxSize)) { wp.clear(); continue; }
        if (mx[0] < anchor[0] - P.reach || mn[0] > anchor[0] + P.reach || mx[2] < anchor[2] - P.reach || mn[2] > anchor[2] + P.reach ||
            mx[1] < anchor[1] - P.reach || mn[1] > anchor[1] + P.reach) { wp.clear(); continue; }
        box[s] = {mn[0], mn[2], mx[0], mx[2]};
        if (!src.bounds) continue;
        lo[0] = std::min(lo[0], std::max(mn[0], anchor[0] - P.reach)); hi[0] = std::max(hi[0], std::min(mx[0], anchor[0] + P.reach));
        lo[1] = std::min(lo[1], std::max(mn[2], anchor[2] - P.reach)); hi[1] = std::max(hi[1], std::min(mx[2], anchor[2] + P.reach));
    }
    for (const auto& sd : seeds) {                                     // the start is always inside
        lo[0] = std::min(lo[0], sd[0] - 2); hi[0] = std::max(hi[0], sd[0] + 2);
        lo[1] = std::min(lo[1], sd[2] - 2); hi[1] = std::max(hi[1], sd[2] + 2);
    }
    if (lo[0] > hi[0]) { if (report) *report = rep; return g; }
    const float R_ox = std::floor(lo[0] / cs) * cs - cs, R_oz = std::floor(lo[1] / cs) * cs - cs;
    const int nx = static_cast<int>(std::ceil((hi[0] - R_ox) / cs)) + 2, nz = static_cast<int>(std::ceil((hi[1] - R_oz) / cs)) + 2;
    const size_t ncell = static_cast<size_t>(nx) * nz;
    // 1. Rasterize: each triangle is told what it is (floor, wall) and which rows it covers; the rows go in bands
    //    of about equal work (a home's geometry is anything but even), rasterized in parallel.
    const float cosSlope = std::cos(P.maxSlope * 3.14159265f / 180);
    struct BTri { uint32_t src, tri; int z0, z1; uint8_t flags, cut; };
    std::vector<BTri> tris;
    std::vector<double> rowWork(nz, 0);
    for (size_t s = 0; s < sources.size(); ++s) {
        const WalkSource& src = sources[s];
        const std::vector<float>& wp = wpos[s];
        if (wp.empty()) continue;
        if (box[s][2] < R_ox || box[s][3] < R_oz || box[s][0] > R_ox + nx * cs || box[s][1] > R_oz + nz * cs) continue;   // outside it all
        for (size_t t = 0; t + 2 < src.indices; t += 3) {
            const uint32_t ia = src.idx[t], ib = src.idx[t + 1], ic = src.idx[t + 2];
            if (ia >= src.verts || ib >= src.verts || ic >= src.verts) continue;
            const float* a = &wp[ia * 3]; const float* bb = &wp[ib * 3]; const float* c = &wp[ic * 3];
            const int z0 = std::max(0, static_cast<int>(std::floor((std::min({a[2], bb[2], c[2]}) - R_oz) / cs)));
            const int z1 = std::min(nz - 1, static_cast<int>(std::floor((std::max({a[2], bb[2], c[2]}) - R_oz) / cs)));
            if (z0 > z1) continue;
            const float e1[3] = {bb[0] - a[0], bb[1] - a[1], bb[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (L < 1e-12f) continue;
            const float up = std::fabs(n[1]) / L;                     // winding varies: a floor faces up either way
            uint8_t fl = 0;
            if (src.role == WorldCollider::Wall || src.role == WorldCollider::Object) fl = kWalkWall;
            else if (src.role == WorldCollider::Floor || src.role == WorldCollider::Walkable) { if (up < 0.5f) continue; fl = kWalkUp | kWalkFloorRole; }   // never blocks
            else if (!src.noFloor && !src.cutout && up >= cosSlope) fl = kWalkUp;   // (foliage is never floor)
            tris.push_back({static_cast<uint32_t>(s), static_cast<uint32_t>(t), z0, z1, fl, static_cast<uint8_t>(src.cutout ? kWalkCutTop | kWalkCutAll : 0)});
            // its work: a call, and a cell per column it crosses in each of its rows
            const float w = (std::max({a[0], bb[0], c[0]}) - std::min({a[0], bb[0], c[0]})) / cs + 2;
            for (int z = z0; z <= z1; ++z) rowWork[z] += w;
        }
    }
    rep.triangles = tris.size();
    const int nb = bandCount(nz);
    std::vector<int> bandStart(nb + 1, nz);
    {
        double total = 0;
        for (double w : rowWork) total += w + 1;
        double acc = 0;
        int b = 0;
        bandStart[0] = 0;
        for (int z = 0; z < nz && b + 1 < nb; ++z) {
            acc += rowWork[z] + 1;
            if (acc >= total * (b + 1) / nb && z + 1 < nz && z + 1 > bandStart[b]) bandStart[++b] = z + 1;
        }
        for (int k = b + 1; k <= nb; ++k) bandStart[k] = nz;
    }
    std::vector<Raster> band(nb);
    std::vector<int> bandOfRow(nz);
    for (int b = 0; b < nb; ++b) {
        Raster& B = band[b];
        B.cs = cs; B.ox = R_ox; B.oz = R_oz; B.z0 = bandStart[b]; B.nx = nx; B.nz = bandStart[b + 1] - bandStart[b];
        B.head.assign(static_cast<size_t>(nx) * B.nz, -1);
        B.pool.reserve(static_cast<size_t>(nx) * B.nz * 2);
        for (int z = bandStart[b]; z < bandStart[b + 1]; ++z) bandOfRow[z] = b;
    }
    auto firstRow = [&](int b) { return bandStart[b]; };
    auto addAt = [&](size_t cell, float l, float h, uint8_t flags, int tag) {
        const int b = bandOfRow[cell / nx];
        band[b].add(static_cast<int>(cell - static_cast<size_t>(firstRow(b)) * nx), l, h, flags, tag);
    };
    std::vector<std::vector<uint32_t>> bandTris(nb);
    for (size_t i = 0; i < tris.size(); ++i)
        for (int b = bandOfRow[tris[i].z0], bl = bandOfRow[tris[i].z1]; b <= bl; ++b) bandTris[b].push_back(static_cast<uint32_t>(i));
    parallelBands(nz, nb, [&](int, int, int b) {
        Raster& B = band[b];
        for (uint32_t i : bandTris[b]) {
            const BTri& bt = tris[i];
            const WalkSource& src = sources[bt.src];
            const float* wp = wpos[bt.src].data();
            B.triangle(&wp[src.idx[bt.tri] * 3], &wp[src.idx[bt.tri + 1] * 3], &wp[src.idx[bt.tri + 2] * 3], bt.flags, src.tag, bt.cut);
        }
        std::vector<uint32_t>().swap(bandTris[b]);
    });
    wpos.clear();
    tris.clear(); tris.shrink_to_fit();
    rep.msRaster = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    // Compact: per cell spans bottom to top.
    struct CSpan { float lo, hi; int tag; uint8_t flags, cut; };
    std::vector<uint32_t> cstart;
    std::vector<CSpan> cs_;
    auto compact = [&] {
        cstart.assign(ncell + 1, 0);
        cs_.clear();
        for (int b = 0; b < nb; ++b) {
            const Raster& B = band[b];
            const size_t base = static_cast<size_t>(firstRow(b)) * nx, n = B.head.size();
            for (size_t i = 0; i < n; ++i) {
                cstart[base + i] = static_cast<uint32_t>(cs_.size());
                for (int s = B.head[i]; s >= 0; s = B.pool[s].next) cs_.push_back({B.pool[s].lo, B.pool[s].hi, B.pool[s].tag, B.pool[s].flags, B.pool[s].cut});
            }
        }
        cstart[ncell] = static_cast<uint32_t>(cs_.size());
    };
    compact();
    // 2. A span is a floor where it's stood on, isn't a wall, and has headroom.
    std::vector<uint8_t> cand;
    auto ceilOf = [&](size_t s, size_t cell) { return s + 1 < cstart[cell + 1] ? cs_[s + 1].lo : 1e30f; };
    auto findCand = [&] {
        cand.assign(cs_.size(), 0);
        for (size_t i = 0; i < ncell; ++i)
            for (uint32_t s = cstart[i]; s < cstart[i + 1]; ++s)
                cand[s] = (cs_[s].flags & kWalkUp) && !(cs_[s].flags & (kWalkWall | kWalkFurniture)) && ceilOf(s, i) - cs_[s].hi >= P.headroom;
    };
    findCand();

    // 3. Bridge seams and small holes: a cell between two floors at about the same height (along x, z or a diagonal,
    //    up to `gap` of missing cells) gets a floor at their height, when nothing stands in the body's way there.
    {
        const int gx = static_cast<int>(std::floor(P.gap / cs + 1e-3f)), gd = static_cast<int>(std::floor(P.gap / (cs * 1.41421f) + 1e-3f));
        struct Add { size_t cell; float h; };
        std::vector<std::vector<Add>> bandAdds(nb);
        auto hasFloorNear = [&](size_t cell, float h, float tol) {
            for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s) if (cand[s] && std::fabs(cs_[s].hi - h) <= tol) return true;
            return false;
        };
        auto blockedAt = [&](size_t cell, float h) {
            for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s) {
                const CSpan& sp = cs_[s];
                if (sp.lo < h + P.headroom && sp.hi > h + P.step) return true;              // something in the body's way
                if ((sp.flags & kWalkWall) && sp.lo < h + P.headroom && sp.hi > h - P.step) return true;
            }
            return false;
        };
        static const int dirs[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
        if (gx > 0)
            parallelBands(nz, nb, [&](int zb0, int zb1, int b) {
                std::vector<Add>& adds = bandAdds[b];
                for (int z = zb0; z < zb1; ++z)
                    for (int x = 0; x < nx; ++x) {
                        const size_t cell = static_cast<size_t>(z) * nx + x;
                        bool any = false;
                        for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s) any |= cand[s] != 0;
                        for (int d = 0; d < 4; ++d) {
                            const int G = d < 2 ? gx : gd;
                            if (G <= 0) continue;
                            auto at = [&](int k, size_t& c) {
                                const int xx = x + dirs[d][0] * k, zz = z + dirs[d][1] * k;
                                if (xx < 0 || zz < 0 || xx >= nx || zz >= nz) return false;
                                c = static_cast<size_t>(zz) * nx + xx;
                                return true;
                            };
                            // a floor behind (-k2), the same floor ahead (+k1), nothing of it between
                            for (int k2 = 1; k2 <= G; ++k2) {
                                size_t c2;
                                if (!at(-k2, c2)) break;
                                for (uint32_t s2 = cstart[c2]; s2 < cstart[c2 + 1]; ++s2) {
                                    if (!cand[s2]) continue;
                                    const float ha = cs_[s2].hi;
                                    if (any && k2 == 1 && hasFloorNear(cell, ha, P.step)) continue;   // no gap here
                                    bool nearer = false;
                                    for (int j = 1; j < k2 && !nearer; ++j) { size_t cj; if (at(-j, cj)) nearer = hasFloorNear(cj, ha, P.step); }
                                    if (nearer) continue;
                                    for (int k1 = 1; k1 <= G - k2 + 1; ++k1) {
                                        size_t c1;
                                        if (!at(k1, c1)) break;
                                        float hb = 0;
                                        bool found = false;
                                        for (uint32_t s1 = cstart[c1]; s1 < cstart[c1 + 1]; ++s1)
                                            if (cand[s1] && std::fabs(cs_[s1].hi - ha) <= P.step) { hb = cs_[s1].hi; found = true; break; }
                                        if (!found) continue;
                                        const float h = ha + (hb - ha) * k2 / (k1 + k2);
                                        if (!(any && hasFloorNear(cell, h, P.step)) && !blockedAt(cell, h)) adds.push_back({cell, h});
                                        break;
                                    }
                                }
                            }
                        }
                    }
            });
        std::vector<Add> adds;
        for (auto& a : bandAdds) adds.insert(adds.end(), a.begin(), a.end());
        if (!adds.empty()) {
            std::sort(adds.begin(), adds.end(), [](const Add& a, const Add& b) { return a.cell != b.cell ? a.cell < b.cell : a.h < b.h; });
            size_t bridged = 0;
            for (size_t i = 0; i < adds.size(); ++i) {
                if (i && adds[i].cell == adds[i - 1].cell && std::fabs(adds[i].h - adds[i - 1].h) < 0.05f) continue;
                addAt(adds[i].cell, adds[i].h, adds[i].h, kWalkUp | kWalkBridged, -1);
                ++bridged;
            }
            rep.bridgedArea = bridged * cs * cs;
            compact();
            findCand();
            for (size_t s = 0; s < cs_.size(); ++s) if (!cand[s]) cs_[s].flags &= static_cast<uint8_t>(~kWalkBridged);
        }
    }
    band.clear(); band.shrink_to_fit();
    rep.msBridge = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() - rep.msRaster;

    // 4. Neighbours (Recast order: -x, +z, +x, -z): a floor next door within a step, with headroom over both.
    static const int ddx[4] = {-1, 0, 1, 0}, ddz[4] = {0, 1, 0, -1};
    const size_t nsp = cs_.size();
    std::vector<std::array<int32_t, 4>> con(nsp, {-1, -1, -1, -1});
    std::vector<int32_t> spanX(nsp), spanZ(nsp);
    parallelBands(nz, nb, [&](int zb0, int zb1, int) {
        for (int z = zb0; z < zb1; ++z)
            for (int x = 0; x < nx; ++x) {
                const size_t cell = static_cast<size_t>(z) * nx + x;
                for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s) {
                    spanX[s] = x; spanZ[s] = z;
                    if (!cand[s]) continue;
                    const float top = ceilOf(s, cell);
                    for (int d = 0; d < 4; ++d) {
                        const int xx = x + ddx[d], zz = z + ddz[d];
                        if (xx < 0 || zz < 0 || xx >= nx || zz >= nz) continue;
                        const size_t c2 = static_cast<size_t>(zz) * nx + xx;
                        float bd = P.step + 1e-4f;
                        for (uint32_t s2 = cstart[c2]; s2 < cstart[c2 + 1]; ++s2) {
                            if (!cand[s2]) continue;
                            const float dh = std::fabs(cs_[s2].hi - cs_[s].hi);
                            if (dh >= bd || std::min(top, ceilOf(s2, c2)) - std::max(cs_[s].hi, cs_[s2].hi) < P.headroom) continue;
                            bd = dh;
                            con[s][d] = static_cast<int32_t>(s2);
                        }
                    }
                }
            }
    });
    // 4b. The tops of things are furniture, not floor: a level top (heights within 5 cm, under 4 m2) whose every
    //     floor next door is at least 12 cm lower — a vase, a coffee table, a sofa's back — is peeled off, and again,
    //     until none is left (the seat under the back, then the footrest under the seat). So is one whose every floor
    //     next door is 12 cm or more higher (a seat you'd only reach by climbing down off the back). Stairs stay: a
    //     tread has floor above and below it. Floor-role surfaces stay. And a step over 23 cm (stairs' risers are 15-22) only
    //     goes onto or off a level area of 4 m2 or more, and 90 cm or more across (twice its area over its outline): a raised
    //     floor is a step up, a sofa's seat or a planter's ledge isn't. (Bumps smaller
    //     than a stair tread aren't counted as floor next door: a cushion's curve or a riser's edge.)
    {
        std::vector<int32_t> plat(nsp, -1);
        std::vector<double> parea, pedge;                              // area, and outline length (m)
        std::vector<uint8_t> pfloorRole;
        std::vector<int32_t> stack;
        for (size_t s0 = 0; s0 < nsp; ++s0) {
            if (!cand[s0] || plat[s0] >= 0) continue;
            const int32_t id = static_cast<int32_t>(parea.size());
            parea.push_back(0);
            pedge.push_back(0);
            pfloorRole.push_back(0);
            plat[s0] = id;
            stack.push_back(static_cast<int32_t>(s0));
            while (!stack.empty()) {
                const int32_t s = stack.back();
                stack.pop_back();
                parea[id] += cs * cs;
                if (cs_[s].flags & kWalkFloorRole) pfloorRole[id] = 1;
                for (int d = 0; d < 4; ++d) {
                    const int32_t b = con[s][d];
                    if (b >= 0 && std::fabs(cs_[b].hi - cs_[s].hi) <= 0.05f) { if (plat[b] < 0) { plat[b] = id; stack.push_back(b); } }
                    else pedge[id] += cs;
                }
            }
        }
        for (size_t s = 0; s < nsp; ++s)
            for (int d = 0; d < 4; ++d) {
                const int32_t b = con[s][d];
                if (b < 0 || plat[s] < 0 || plat[b] < 0 || plat[s] == plat[b]) continue;
                const float dh = std::fabs(cs_[b].hi - cs_[s].hi);
                const double small = std::min(parea[plat[s]], parea[plat[b]]);
                const double across = std::min(2 * parea[plat[s]] / std::max(cs, static_cast<float>(pedge[plat[s]])), 2 * parea[plat[b]] / std::max(cs, static_cast<float>(pedge[plat[b]])));
                if (dh > 0.23f && (small < 4.0 || across < 0.9) && !pfloorRole[plat[s]] && !pfloorRole[plat[b]])
                    con[s][d] = -1;                                    // (both ways: the other side meets this too)
            }
        std::vector<uint8_t> peeled(parea.size(), 0);
        for (int round = 0; round < 12; ++round) {
            // per plateau: any live floor next door, and is every one of them 12 cm or more below?
            std::vector<uint8_t> hasNb(parea.size(), 0), allLower(parea.size(), 1), allHigher(parea.size(), 1);
            for (size_t s = 0; s < nsp; ++s) {
                if (plat[s] < 0 || peeled[plat[s]]) continue;
                for (int d = 0; d < 4; ++d) {
                    const int32_t b = con[s][d];
                    if (b < 0 || plat[b] < 0 || plat[b] == plat[s] || peeled[plat[b]] || parea[plat[b]] < 0.08) continue;   // (a bump isn't a floor)
                    hasNb[plat[s]] = 1;
                    if (cs_[b].hi > cs_[s].hi - 0.12f) allLower[plat[s]] = 0;
                    if (cs_[b].hi < cs_[s].hi + 0.12f) allHigher[plat[s]] = 0;
                }
            }
            bool any = false;
            for (size_t p = 0; p < parea.size(); ++p)
                if (!peeled[p] && hasNb[p] && (allLower[p] || allHigher[p]) && parea[p] < 4.0 && !pfloorRole[p]) { peeled[p] = 1; any = true; }
            if (!any) break;
        }
        for (size_t s = 0; s < nsp; ++s)
            if (plat[s] >= 0 && peeled[plat[s]]) { cand[s] = 0; cs_[s].flags |= kWalkFurniture; }
        for (size_t s = 0; s < nsp; ++s)                               // links to what's gone
            for (int d = 0; d < 4; ++d) if (con[s][d] >= 0 && !cand[con[s][d]]) con[s][d] = -1;
    }
    // 5. Each floor's distance to the area's edge (Euclidean; cell units here): the edge is the side of a floor
    //    with no floor next door, or the corner where only the diagonal one is missing. Each floor takes the
    //    nearest edge point its neighbours know of, in two sweeps each way (8SSEDT over the floors' links, so
    //    stacked floors and steps work). Floors at least the body radius in are the ones to stand on; Floor-role
    //    surfaces stay whole.
    std::vector<float> dist(nsp, 1e30f), siteX(nsp, 0), siteZ(nsp, 0);   // squared here (cells²)
    std::vector<float> siteNX(nsp, 0), siteNZ(nsp, 0);                // the edge's line there (its normal; 0: a point)
    // the squared distance from a cell centre to an edge point: to its line, within a cell of it along the line
    auto edgeDist = [&](float cx, float cz, float px, float pz, float nx, float nz) {
        const float ex = px - cx, ez = pz - cz, dd = ex * ex + ez * ez;
        if (nx == 0 && nz == 0) return dd;
        const float across = ex * nx + ez * nz, along2 = dd - across * across;
        return along2 <= 1.0f ? across * across : dd;                  // (past its own stretch: the point, as before)
    };
    {
        // The edge points: each floor's sides with no floor beyond, as the cells' staircase.
        struct Site { float x, z; uint32_t s; int8_t nx, nz; float lx = 0, lz = 0; };   // n: the way out of the floor there; l: its line's normal
        std::vector<Site> sites;
        for (size_t s = 0; s < nsp; ++s) {
            if (!cand[s]) continue;
            const float cx = spanX[s] + 0.5f, cz = spanZ[s] + 0.5f;
            for (int d = 0; d < 4; ++d) {
                if (con[s][d] < 0) { sites.push_back({cx + 0.5f * ddx[d], cz + 0.5f * ddz[d], static_cast<uint32_t>(s), static_cast<int8_t>(ddx[d]), static_cast<int8_t>(ddz[d])}); continue; }
                const int d2 = (d + 1) & 3;                            // the corner between d and the next direction
                if (con[s][d2] < 0) continue;
                if (con[con[s][d]][d2] < 0 && con[con[s][d2]][d] < 0)
                    sites.push_back({cx + 0.5f * (ddx[d] + ddx[d2]), cz + 0.5f * (ddz[d] + ddz[d2]), static_cast<uint32_t>(s),
                                     static_cast<int8_t>(ddx[d] + ddx[d2]), static_cast<int8_t>(ddz[d] + ddz[d2])});
            }
        }
        // A wall at an angle makes that staircase a zigzag: where the edge points within 2.5 cells (on the same level)
        // lie along a line, each moves onto it, so a straight wall gives a straight edge. Corners and curves aren't a
        // line (their spread isn't one-sided) and stay as they are.
        // the points by cell (a flat index: cellStart[c] .. cellStart[c + 1] in byCell)
        const size_t ncells = static_cast<size_t>(nx) * nz;
        auto cellOfSite = [&](const Site& t) {
            const int x = std::clamp(static_cast<int>(std::floor(t.x)), 0, nx - 1), z = std::clamp(static_cast<int>(std::floor(t.z)), 0, nz - 1);
            return static_cast<size_t>(z) * nx + x;
        };
        std::vector<uint32_t> cellStart(ncells + 1, 0), byCell(sites.size());
        for (const Site& t : sites) ++cellStart[cellOfSite(t) + 1];
        for (size_t c = 0; c < ncells; ++c) cellStart[c + 1] += cellStart[c];
        {
            std::vector<uint32_t> fill(cellStart.begin(), cellStart.end() - 1);
            for (uint32_t i = 0; i < sites.size(); ++i) byCell[fill[cellOfSite(sites[i])]++] = i;
        }
        std::vector<uint32_t> near;
        std::vector<Site> fitted = sites;
        constexpr float kR = 3.5f;
        std::vector<uint32_t> mark(nsp, 0);                            // floors within 4 cells of the point's own, over the floor
        std::vector<uint32_t> ring, nextRing;
        uint32_t epoch = 0;
        for (uint32_t i = 0; i < sites.size(); ++i) {
            const Site& a = sites[i];
            const float ha = cs_[a.s].hi;
            ++epoch;
            ring.assign(1, a.s);
            mark[a.s] = epoch;
            for (int k = 0; k < 5; ++k) {                              // (the floor past a thin wall isn't this edge's)
                nextRing.clear();
                for (uint32_t q : ring)
                    for (int d = 0; d < 4; ++d) {
                        const int32_t b = con[q][d];
                        if (b >= 0 && cand[b] && mark[b] != epoch) { mark[b] = epoch; nextRing.push_back(static_cast<uint32_t>(b)); }
                    }
                ring.swap(nextRing);
            }
            double n = 0, mx = 0, mz = 0;
            near.clear();
            for (int bz = std::max(0, static_cast<int>(std::floor(a.z - kR))); bz <= std::min(nz - 1, static_cast<int>(std::floor(a.z + kR))); ++bz)
                for (int bx = std::max(0, static_cast<int>(std::floor(a.x - kR))); bx <= std::min(nx - 1, static_cast<int>(std::floor(a.x + kR))); ++bx) {
                    const size_t c = static_cast<size_t>(bz) * nx + bx;
                    for (uint32_t k = cellStart[c]; k < cellStart[c + 1]; ++k) {
                        const uint32_t j = byCell[k];
                        const Site& b = sites[j];
                        if ((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z) > kR * kR || std::fabs(cs_[b.s].hi - ha) > P.step) continue;
                        if (mark[b.s] != epoch || b.nx * a.nx + b.nz * a.nz < 0) continue;   // another edge (past a wall)
                        near.push_back(j); n += 1; mx += b.x; mz += b.z;
                    }
                }
            if (n < 6) continue;
            mx /= n; mz /= n;
            double sxx = 0, szz = 0, sxz = 0;
            for (uint32_t j : near) { const double dx = sites[j].x - mx, dz = sites[j].z - mz; sxx += dx * dx; szz += dz * dz; sxz += dx * dz; }
            const double tr = sxx + szz, det = sxx * szz - sxz * sxz, disc = std::sqrt(std::max(0.0, tr * tr / 4 - det));
            const double l1 = tr / 2 + disc, l2 = tr / 2 - disc;          // spread along the line, and across it
            if (l1 <= 1e-9 || l2 / l1 > 0.06 || l2 / n > 0.2) continue; // not a line (a corner, a curve), or not a thin one
            double ux = sxz, uz = l1 - sxx;                            // the line's direction (the big eigenvector)
            if (std::fabs(ux) + std::fabs(uz) < 1e-12) { ux = sxx >= szz ? 1 : 0; uz = sxx >= szz ? 0 : 1; }
            const double ul = std::sqrt(ux * ux + uz * uz);
            ux /= ul; uz /= ul;
            const double along = (a.x - mx) * ux + (a.z - mz) * uz;
            fitted[i].lx = static_cast<float>(-uz); fitted[i].lz = static_cast<float>(ux);
            fitted[i].x = static_cast<float>(mx + ux * along);
            fitted[i].z = static_cast<float>(mz + uz * along);
        }
        for (const Site& f : fitted) {
            const float dd = edgeDist(spanX[f.s] + 0.5f, spanZ[f.s] + 0.5f, f.x, f.z, f.lx, f.lz);
            if (dd < dist[f.s]) { dist[f.s] = dd; siteX[f.s] = f.x; siteZ[f.s] = f.z; siteNX[f.s] = f.lx; siteNZ[f.s] = f.lz; }
        }
    }
    {
        auto take = [&](size_t s, int32_t from) {
            if (from < 0 || dist[from] >= 1e29f) return;
            const float dd = edgeDist(spanX[s] + 0.5f, spanZ[s] + 0.5f, siteX[from], siteZ[from], siteNX[from], siteNZ[from]);
            if (dd < dist[s]) { dist[s] = dd; siteX[s] = siteX[from]; siteZ[s] = siteZ[from]; siteNX[s] = siteNX[from]; siteNZ[s] = siteNZ[from]; }
        };
        auto via = [&](size_t s, int a, int b) { const int32_t m = con[s][a]; return m >= 0 ? con[m][b] : -1; };
        // spans are stored row by row, x ascending: a row is one index range
        auto row = [&](int z, bool rightward, auto&& fn) {
            const uint32_t r0 = cstart[static_cast<size_t>(z) * nx], r1 = cstart[static_cast<size_t>(z + 1) * nx];
            if (rightward) { for (uint32_t s = r0; s < r1; ++s) if (cand[s]) fn(s); }
            else for (uint32_t s = r1; s-- > r0;) if (cand[s]) fn(s);
        };
        for (int pass = 0; pass < 2; ++pass) {
            for (int z = 0; z < nz; ++z) {                             // down the rows: from -z, -x and their diagonals
                row(z, true, [&](size_t s) { take(s, con[s][0]); take(s, con[s][3]); take(s, via(s, 3, 0)); take(s, via(s, 0, 3)); take(s, via(s, 3, 2)); take(s, via(s, 2, 3)); });
                row(z, false, [&](size_t s) { take(s, con[s][2]); });
            }
            for (int z = nz - 1; z >= 0; --z) {                        // and back up: from +z, +x and theirs
                row(z, false, [&](size_t s) { take(s, con[s][2]); take(s, con[s][1]); take(s, via(s, 1, 2)); take(s, via(s, 2, 1)); take(s, via(s, 1, 0)); take(s, via(s, 0, 1)); });
                row(z, true, [&](size_t s) { take(s, con[s][0]); });
            }
        }
    }
    // A wall (a Wall / Object surface: an invisible wall) within a body's radius of a span, in the body's way there.
    const int wallReach = static_cast<int>(std::ceil(P.radius / cs));
    auto wallNear = [&](size_t s) {
        const float h = cs_[s].hi, reach = P.radius + cs;
        for (int dz = -wallReach; dz <= wallReach; ++dz)
            for (int dx = -wallReach; dx <= wallReach; ++dx) {
                const int x = spanX[s] + dx, z = spanZ[s] + dz;
                if (x < 0 || z < 0 || x >= nx || z >= nz || static_cast<float>(dx * dx + dz * dz) * cs * cs > reach * reach) continue;
                const size_t cell = static_cast<size_t>(z) * nx + x;
                for (uint32_t t = cstart[cell]; t < cstart[cell + 1]; ++t)
                    if ((cs_[t].flags & kWalkWall) && cs_[t].lo < h + P.headroom && cs_[t].hi > h - P.step) return true;
            }
        return false;
    };
    std::vector<uint8_t> core(nsp, 0);
    for (size_t s = 0; s < nsp; ++s) {
        if (!cand[s]) { dist[s] = 0; continue; }
        dist[s] = std::sqrt(std::min(dist[s], 1e8f)) * cs;
        // A drawn floor's edge is where it ends (walkable right up to it), but a wall standing on it still keeps
        // the body its radius away: one drawn through it takes that floor away.
        if ((cs_[s].flags & kWalkFloorRole) && !wallNear(s)) dist[s] = std::max(dist[s], 2 * P.radius);
        core[s] = dist[s] >= P.radius - 1e-4f;
    }
    // 6. Separate areas; keep the ones the player can get to.
    std::vector<int32_t> region(nsp, -1);
    std::vector<double> area;
    std::vector<uint8_t> keep;
    {
        std::vector<int32_t> stack;
        for (size_t s0 = 0; s0 < nsp; ++s0) {
            if (!core[s0] || region[s0] >= 0) continue;
            const int32_t id = static_cast<int32_t>(area.size());
            area.push_back(0);
            keep.push_back(0);
            region[s0] = id;
            stack.push_back(static_cast<int32_t>(s0));
            while (!stack.empty()) {
                const int32_t s = stack.back();
                stack.pop_back();
                area[id] += cs * cs;
                if (cs_[s].flags & kWalkFloorRole) keep[id] = 1;
                for (int d = 0; d < 4; ++d) {
                    const int32_t b = con[s][d];
                    if (b >= 0 && core[b] && region[b] < 0) { region[b] = id; stack.push_back(b); }
                }
            }
        }
        // seeds: the area under each (searched up to 1 m round it)
        std::vector<std::array<float, 3>> sd = seeds;
        if (sd.empty()) sd.push_back({anchor[0], anchor[1], anchor[2]});
        bool seeded = false;
        for (const auto& p : sd) {
            const int cx = static_cast<int>(std::floor((p[0] - R_ox) / cs)), cz = static_cast<int>(std::floor((p[2] - R_oz) / cs));
            const int r = static_cast<int>(std::ceil(1.0f / cs));
            float best = 1e30f;
            int32_t bs = -1;
            for (int z = cz - r; z <= cz + r; ++z)
                for (int x = cx - r; x <= cx + r; ++x) {
                    if (x < 0 || z < 0 || x >= nx || z >= nz) continue;
                    const size_t cell = static_cast<size_t>(z) * nx + x;
                    for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s) {
                        if (!core[s] || std::fabs(cs_[s].hi - p[1]) > 0.6f) continue;
                        const float dd = std::hypot((x - cx) * cs, (z - cz) * cs) + std::fabs(cs_[s].hi - p[1]);
                        if (dd < best) { best = dd; bs = static_cast<int32_t>(s); }
                    }
                }
            if (bs >= 0) { keep[region[bs]] |= 2; seeded = true; }
        }
        if (!seeded && !area.empty()) keep[std::max_element(area.begin(), area.end()) - area.begin()] |= 2;
        for (size_t r = 0; r < area.size(); ++r) {
            if (keep[r] == 1 && area[r] < P.minArea) keep[r] = 0;      // a Floor-role speck nobody starts on
            if (keep[r]) { ++rep.areas; rep.walkArea += area[r]; } else { ++rep.unreached; rep.unreachedArea += area[r]; }
        }
    }
    // The floors round a kept area that are too near its edge to stand on: the edge runs over them.
    std::vector<uint8_t> nearEdge(nsp, 0);
    {
        std::vector<int32_t> ring, nextRing;
        for (size_t s = 0; s < nsp; ++s) if (core[s] && keep[region[s]]) ring.push_back(static_cast<int32_t>(s));
        const int depth = static_cast<int>(std::ceil(P.radius / cs)) + 1;
        for (int k = 0; k < depth && !ring.empty(); ++k) {
            nextRing.clear();
            for (int32_t s : ring)
                for (int d = 0; d < 4; ++d) {
                    const int32_t b = con[s][d];
                    if (b >= 0 && cand[b] && !core[b] && !nearEdge[b]) { nearEdge[b] = 1; nextRing.push_back(b); }
                }
            ring.swap(nextRing);
        }
    }
    for (size_t s = 0; s < nsp; ++s) {
        cs_[s].flags &= static_cast<uint8_t>(~(kWalkOk | kWalkUnreached | kWalkNear));
        if (core[s]) cs_[s].flags |= keep[region[s]] ? kWalkOk : kWalkUnreached;
        else if (nearEdge[s]) cs_[s].flags |= kWalkNear;
        else cs_[s].flags &= static_cast<uint8_t>(~kWalkBridged);
    }
    // 7. What stops you at the area's edges (the report): the first thing in the way beyond each edge cell.
    if (report) {
        std::vector<int> hits;
        const int reachCells = static_cast<int>(std::ceil(P.radius / cs)) + 3;
        for (int z = 0; z < nz; ++z)
            for (int x = 0; x < nx; ++x) {
                const size_t cell = static_cast<size_t>(z) * nx + x;
                for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s) {
                    if (!(cs_[s].flags & kWalkOk)) continue;
                    for (int d = 0; d < 4; ++d) {
                        const int32_t b = con[s][d];
                        if (b >= 0 && (cs_[b].flags & kWalkOk)) continue;
                        const float h = cs_[s].hi;
                        int tag = -2;
                        for (int k = 1; k <= reachCells && tag == -2; ++k) {
                            const int xx = x + ddx[d] * k, zz = z + ddz[d] * k;
                            if (xx < 0 || zz < 0 || xx >= nx || zz >= nz) break;
                            const size_t c2 = static_cast<size_t>(zz) * nx + xx;
                            for (uint32_t s2 = cstart[c2]; s2 < cstart[c2 + 1]; ++s2) {
                                const CSpan& sp = cs_[s2];
                                if ((sp.lo < h + P.headroom && sp.hi > h + P.step) || ((sp.flags & kWalkWall) && sp.lo < h + P.headroom && sp.hi > h - P.step)) { tag = sp.tag; break; }
                            }
                        }
                        if (tag == -2) ++rep.ledgeCells;
                        else if (tag >= 0) { if (static_cast<size_t>(tag) >= hits.size()) hits.resize(tag + 1, 0); ++hits[tag]; }
                        break;                                         // one per edge cell
                    }
                }
            }
        for (size_t t = 0; t < hits.size(); ++t) if (hits[t]) rep.blockers.emplace_back(static_cast<int>(t), hits[t]);
        std::sort(rep.blockers.begin(), rep.blockers.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    }
    // 8. Keep the part round the area (3 m more, for teleport arcs to hit its walls).
    int bx0 = nx, bx1 = -1, bz0 = nz, bz1 = -1;
    for (int z = 0; z < nz; ++z)
        for (int x = 0; x < nx; ++x) {
            const size_t cell = static_cast<size_t>(z) * nx + x;
            for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s)
                if (cs_[s].flags & (kWalkOk | kWalkNear)) { bx0 = std::min(bx0, x); bx1 = std::max(bx1, x); bz0 = std::min(bz0, z); bz1 = std::max(bz1, z); }
        }
    if (bx1 < 0) { bx0 = bz0 = 0; bx1 = bz1 = -1; }
    else {
        const int m = static_cast<int>(std::ceil(3.0f / cs));
        bx0 = std::max(0, bx0 - m); bz0 = std::max(0, bz0 - m); bx1 = std::min(nx - 1, bx1 + m); bz1 = std::min(nz - 1, bz1 + m);
    }
    g->ox = R_ox + bx0 * cs; g->oz = R_oz + bz0 * cs;
    g->nx = bx1 - bx0 + 1; g->nz = bz1 - bz0 + 1;
    g->start.assign(static_cast<size_t>(g->nx) * g->nz + 1, 0);
    for (int z = 0; z < g->nz; ++z)
        for (int x = 0; x < g->nx; ++x) {
            g->start[static_cast<size_t>(z) * g->nx + x] = static_cast<uint32_t>(g->spans.size());
            const size_t cell = static_cast<size_t>(z + bz0) * nx + (x + bx0);
            for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s)
                g->spans.push_back({cs_[s].lo, cs_[s].hi, cs_[s].flags, static_cast<uint8_t>(std::min(255L, std::lround(dist[s] * 100))), 0, cs_[s].cut});
        }
    g->start.back() = static_cast<uint32_t>(g->spans.size());
    g->walkArea = rep.walkArea;
    g->smoothHeights();
    rep.spans = g->spans.size();
    rep.nx = g->nx; rep.nz = g->nz;
    rep.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (report) *report = std::move(rep);
    return g;
}

// ── Saving (world.json "walkGrid": deflated, base64) ── FWG5: + each span's cut; FWG4: + inset; FWG3: + metaBounds;
// FWG2: + each floor's distance to the edge.

namespace walkgrid_detail {

inline void varint(std::vector<uint8_t>& o, uint32_t v) { while (v >= 0x80) { o.push_back(static_cast<uint8_t>(v | 0x80)); v >>= 7; } o.push_back(static_cast<uint8_t>(v)); }
inline uint32_t zig(int32_t v) { return (static_cast<uint32_t>(v) << 1) ^ static_cast<uint32_t>(v >> 31); }
inline int32_t unzig(uint32_t v) { return static_cast<int32_t>(v >> 1) ^ -static_cast<int32_t>(v & 1); }
constexpr float kQ = 0.005f;            // heights are kept to 5 mm

inline std::string b64(const std::vector<uint8_t>& d) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((d.size() + 2) / 3 * 4);
    for (size_t i = 0; i < d.size(); i += 3) {
        const uint32_t v = (d[i] << 16) | ((i + 1 < d.size() ? d[i + 1] : 0) << 8) | (i + 2 < d.size() ? d[i + 2] : 0);
        o += k[(v >> 18) & 63]; o += k[(v >> 12) & 63];
        o += i + 1 < d.size() ? k[(v >> 6) & 63] : '=';
        o += i + 2 < d.size() ? k[v & 63] : '=';
    }
    return o;
}
inline bool unb64(const std::string& s, std::vector<uint8_t>& o) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    o.clear();
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) { bits -= 8; o.push_back(static_cast<uint8_t>(acc >> bits)); }
    }
    return true;
}

}  // namespace walkgrid_detail

inline std::string encodeWalkGrid(const WalkGrid& g) {
    using namespace walkgrid_detail;
    std::vector<uint8_t> raw;
    auto f32 = [&](float v) { uint8_t b[4]; std::memcpy(b, &v, 4); raw.insert(raw.end(), b, b + 4); };
    raw.insert(raw.end(), {'F', 'W', 'G', '5'});
    for (float v : {g.p.cell, g.p.step, g.p.headroom, g.p.radius, g.p.gap, g.p.maxSlope, g.p.minArea, g.p.reach, g.ox, g.oz, static_cast<float>(g.walkArea), g.p.metaBounds, g.p.inset}) f32(v);
    varint(raw, static_cast<uint32_t>(g.nx)); varint(raw, static_cast<uint32_t>(g.nz));
    int32_t prevFirst = 0;
    for (size_t c = 0; c + 1 < g.start.size(); ++c) {
        varint(raw, g.start[c + 1] - g.start[c]);
        int32_t prev = prevFirst;
        for (uint32_t s = g.start[c]; s < g.start[c + 1]; ++s) {
            const int32_t lo = static_cast<int32_t>(std::lround(g.spans[s].lo / kQ)), hi = static_cast<int32_t>(std::lround(g.spans[s].hi / kQ));
            if (s == g.start[c]) prevFirst = lo;
            varint(raw, zig(lo - prev));
            varint(raw, static_cast<uint32_t>(std::max(0, hi - lo)));
            raw.push_back(g.spans[s].flags);
            raw.push_back(g.spans[s].dist);
            raw.push_back(g.spans[s].cut);
            prev = hi;
        }
    }
    mz_ulong n = mz_compressBound(static_cast<mz_ulong>(raw.size()));
    std::vector<uint8_t> z(n + 4);
    const uint32_t rawSize = static_cast<uint32_t>(raw.size());
    std::memcpy(z.data(), &rawSize, 4);
    if (mz_compress2(z.data() + 4, &n, raw.data(), static_cast<mz_ulong>(raw.size()), 6) != MZ_OK) return "";
    z.resize(n + 4);
    return b64(z);
}

inline std::shared_ptr<WalkGrid> decodeWalkGrid(const std::string& text) {
    using namespace walkgrid_detail;
    std::vector<uint8_t> z;
    if (text.empty() || !unb64(text, z) || z.size() < 8) return nullptr;
    uint32_t rawSize;
    std::memcpy(&rawSize, z.data(), 4);
    // (deflate packs at most ~1032:1: a header claiming more is damaged, and isn't allocated)
    if (rawSize > (1u << 30) || rawSize > (z.size() - 4) * 1100) return nullptr;
    std::vector<uint8_t> raw(rawSize);
    mz_ulong n = rawSize;
    if (mz_uncompress(raw.data(), &n, z.data() + 4, static_cast<mz_ulong>(z.size() - 4)) != MZ_OK || n != rawSize) return nullptr;
    size_t at = 0;
    bool bad = false;
    auto f32 = [&] { float v = 0; if (at + 4 <= raw.size()) std::memcpy(&v, &raw[at], 4); else bad = true; at += 4; return v; };
    auto vi = [&] {
        uint32_t v = 0;
        for (int sh = 0; sh < 35; sh += 7) {
            if (at >= raw.size()) { bad = true; return v; }
            const uint8_t b = raw[at++];
            v |= static_cast<uint32_t>(b & 0x7f) << sh;
            if (!(b & 0x80)) return v;
        }
        bad = true;
        return v;
    };
    if (raw.size() < 4 || std::memcmp(raw.data(), "FWG", 3) != 0 || raw[3] < '1' || raw[3] > '5') return nullptr;
    const bool v1 = raw[3] == '1';
    at = 4;
    auto g = std::make_shared<WalkGrid>();
    g->p.cell = f32(); g->p.step = f32(); g->p.headroom = f32(); g->p.radius = f32(); g->p.gap = f32(); g->p.maxSlope = f32();
    g->p.minArea = f32(); g->p.reach = f32(); g->ox = f32(); g->oz = f32(); g->walkArea = f32();
    g->p.metaBounds = raw[3] >= '3' ? f32() : 1.0f;                    // made before walking past them was a choice
    g->p.inset = raw[3] >= '4' ? f32() : g->p.radius;                  // made when you stopped a body radius short
    g->nx = static_cast<int>(vi()); g->nz = static_cast<int>(vi());
    // NaN fails every comparison: checked as "is finite" so it can't slip through to the floor queries' casts. Each
    // cell takes at least a byte of what's left.
    if (bad || !(g->p.cell >= 0.01f && g->p.cell <= 100.0f) || !std::isfinite(g->ox) || !std::isfinite(g->oz) ||
        std::fabs(g->ox) > 1e6f || std::fabs(g->oz) > 1e6f || g->nx < 0 || g->nz < 0 ||
        static_cast<uint64_t>(g->nx) * g->nz > (1u << 26) || static_cast<uint64_t>(g->nx) * g->nz > raw.size() - at)
        return nullptr;
    const size_t cells = static_cast<size_t>(g->nx) * g->nz;
    g->start.resize(cells + 1);
    int32_t prevFirst = 0;
    for (size_t c = 0; c < cells && !bad; ++c) {
        g->start[c] = static_cast<uint32_t>(g->spans.size());
        const uint32_t k = vi();
        if (k > 4096) { bad = true; break; }
        int32_t prev = prevFirst;
        for (uint32_t i = 0; i < k && !bad; ++i) {
            const int32_t lo = prev + unzig(vi()), hi = lo + static_cast<int32_t>(vi());
            if (at >= raw.size()) { bad = true; break; }
            const uint8_t fl = raw[at++];
            uint8_t dist = 0;
            if (!v1) { if (at >= raw.size()) { bad = true; break; } dist = raw[at++]; }
            // FWG1 knew only walkable or not: twice the radius in puts the edge halfway between cells, as it was
            else if (fl & kWalkOk) dist = static_cast<uint8_t>(std::min(255L, std::lround(g->p.radius * 200)));
            uint8_t cut = 0;
            if (raw[3] >= '5') { if (at >= raw.size()) { bad = true; break; } cut = raw[at++]; }
            if (i == 0) prevFirst = lo;
            g->spans.push_back({lo * kQ, hi * kQ, fl, dist, 0, cut});
            prev = hi;
        }
    }
    if (bad) return nullptr;
    g->start[cells] = static_cast<uint32_t>(g->spans.size());
    g->smoothHeights();
    return g;
}

// The settings a saved walkable area was made with (Studio's sliders); false when there's none.
inline bool walkGridParams(const std::string& text, WalkParams& p) {
    std::shared_ptr<WalkGrid> g = decodeWalkGrid(text);
    if (!g) return false;
    p = g->p;
    return true;
}

// The seeds of a world: its spawns, hotspots and the spots seats let you out at.
inline std::vector<std::array<float, 3>> walkSeeds(const HomeWorld& w) {
    std::vector<std::array<float, 3>> s;
    for (const WorldPoint& p : w.spawns) s.push_back({p.pos[0], p.pos[1], p.pos[2]});
    for (const WorldPoint& p : w.hotspots) s.push_back({p.pos[0], p.pos[1], p.pos[2]});
    for (const WorldPoint& p : w.seats) {
        // the exit is relative to the seat, turned by its yaw; it's on the floor
        // (as Locomotion::standUp: half a metre forward when the seat names none)
        const float c = std::cos(p.yaw), sn = std::sin(p.yaw);
        float lx = p.exit[0], lz = p.exit[2];
        if (std::hypot(lx, lz) < 0.05f) { lx = 0; lz = -0.5f; }
        s.push_back({p.pos[0] + c * lx + sn * lz, p.pos[1] - 0.45f, p.pos[2] - sn * lx + c * lz});
    }
    return s;
}

// The world's colliders as walk sources (box / sphere / capsule triangulated into `store`, which must outlive the
// sources). skipMeshKeys: nothing; the caller leaves render geometry out itself.
inline void walkColliderSources(const HomeWorld& w, std::vector<WalkSource>& out, std::vector<std::vector<float>>& storeP,
                                std::vector<std::vector<uint32_t>>& storeI, int tagBase = -1) {
    storeP.reserve(storeP.size() + w.colliders.size());
    storeI.reserve(storeI.size() + w.colliders.size());
    for (size_t i = 0; i < w.colliders.size(); ++i) {
        const WorldCollider& c = w.colliders[i];
        if (c.role == WorldCollider::Trigger) continue;
        WalkSource s;
        s.role = c.role;
        s.tag = tagBase >= 0 ? tagBase + static_cast<int>(i) : -1;
        if (c.kind == WorldCollider::Mesh) {
            if (c.positions.empty() || c.indices.empty()) continue;
            s.pos = c.positions.data(); s.verts = c.positions.size() / 3; s.idx = c.indices.data(); s.indices = c.indices.size();
            std::copy_n(c.world, 16, s.world);
        } else {
            storeP.emplace_back(); storeI.emplace_back();
            walkShapeMesh(c, storeP.back(), storeI.back());
            if (storeI.back().empty()) continue;
            if (c.role == WorldCollider::Floor && c.kind == WorldCollider::Box) {}   // its top faces up: floor; sides skipped (Floor never blocks)
            s.pos = storeP.back().data(); s.verts = storeP.back().size() / 3; s.idx = storeI.back().data(); s.indices = storeI.back().size();
        }
        out.push_back(s);
    }
}
