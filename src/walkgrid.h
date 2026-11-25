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
// A home walked on its own collision (Meta's colliders: buildWalkGrid's homeFloor) is exactly that floor, but for the
// tops of named seats and tables: nothing bridged, trimmed by the body or peeled off by shape, its edges where its
// colliders really end, not on the cells.
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
#include <initializer_list>
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
// and a blocker a blocker wherever the word is, and a rock's shelf or ledge is rock ("Large_Rock_Shelf").
// A name's words, lower case: split at anything not a letter and where a capital follows a small letter
// ("deskSeatCushion_02" -> desk, seat, cushion).
inline std::vector<std::string> walkNameWords(const std::string& name) {
    std::vector<std::string> words;
    std::string w;
    for (size_t i = 0; i <= name.size(); ++i) {
        const char c = i < name.size() ? name[i] : '_';
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        const bool upper = c >= 'A' && c <= 'Z';
        if (!alpha || (upper && !w.empty() && !(name[i - 1] >= 'A' && name[i - 1] <= 'Z'))) { if (!w.empty()) words.push_back(w); w.clear(); }
        if (alpha) w += static_cast<char>(upper ? c - 'A' + 'a' : c);
    }
    return words;
}

// Whether a name has one of `roots` as a word, or its plural ("tree", "trees"; "bush", "bushes").
inline bool walkNameHasWord(const std::vector<std::string>& words, std::initializer_list<const char*> roots) {
    for (const std::string& t : words)
        for (const char* r : roots) {
            const size_t n = std::strlen(r);
            if (t.compare(0, n, r) != 0) continue;
            if (t.size() == n || (t.size() == n + 1 && t[n] == 's') || (t.size() == n + 2 && t[n] == 'e' && t[n + 1] == 's')) return true;
        }
    return false;
}

// Whether a name says it's a plant: a plant word starting or ending one of its words, as names glue them on
// ("fgtree02", "palmTree", "plantfoliage", "grassClump"), but not the few words that only look so ("street", "ravine").
// soft: only what you'd walk through (grass, ferns, flowers, leaves, a bush, weeds), not trees, palms or branches
// (wood: a trunk, a fallen log), and nothing in a pot or a planter.
inline bool walkNameIsPlant(const std::vector<std::string>& words, bool soft = false) {
    static const char* const kSoft[] = {"plant", "leaf", "leaves", "foliage", "grass", "flower", "fern", "frond", "bush", "shrub",
                                        "hedge", "vine", "ivy", "moss", "reed", "weed", "clover", "petal", "bracken", "vege"};
    static const char* const kWoody[] = {"tree", "palm", "branch"};
    static const char* const kNot[] = {"street", "streets", "ravine", "ravines", "divine", "ambush", "tweed", "freed", "greed", "agreed",
                                       "treehouse", "treehouses"};
    static const char* const kHard[] = {"planter", "pot", "vase", "trunk", "log", "stump", "bark", "wood"};
    auto hasRoot = [](const std::string& t, const char* r) {
        const size_t n = std::strlen(r);
        if (t.size() >= n && (t.compare(0, n, r) == 0 || t.compare(t.size() - n, n, r) == 0)) return true;
        return t.size() > n && t.compare(t.size() - n - 1, n, r) == 0 && t.back() == 's';   // "fgtrees"
    };
    auto isNot = [&](const std::string& t) {
        return std::find_if(std::begin(kNot), std::end(kNot), [&](const char* n) { return t == n; }) != std::end(kNot);
    };
    if (soft)
        for (const std::string& t : words)
            if (!isNot(t)) for (const char* r : kHard) if (hasRoot(t, r)) return false;
    for (const std::string& t : words) {
        if (isNot(t)) continue;
        for (const char* r : kSoft) if (hasRoot(t, r) && !(soft && t.compare(0, 7, "planter") == 0)) return true;
        if (!soft) for (const char* r : kWoody) if (hasRoot(t, r)) return true;
    }
    return false;
}

// A see-through (cut-out) mesh that's built, not grown: a fence, a railing, a gate, a grille. It blocks at any height,
// as solid things do; low cut-out plants are walked through.
inline bool walkNameIsStructure(const std::string& name) {
    return walkNameHasWord(walkNameWords(name), {"fence", "rail", "railing", "balustrade", "baluster", "banister", "handrail", "gate",
                                                 "lattice", "trellis", "grate", "grating", "grille", "grill", "bar", "net", "netting",
                                                 "screen", "cage", "barrier", "post"});
}

inline WalkHint walkNameHint(const std::string& full, const std::string& prefix = {}) {
    const std::string name = full.compare(0, prefix.size(), prefix) == 0 ? full.substr(prefix.size()) : full;
    const std::vector<std::string> words = walkNameWords(name);
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
    bool planter = false, blocker = false, rock = false, shelf = false;
    for (const std::string& t : words) {
        planter |= t == "planter" || t == "pot";
        rock |= t == "rock" || t == "rocks" || t == "boulder" || t == "cliff" || t == "stone";
        blocker |= t == "blocker";
        for (const auto& k : kWords)
            if (t == k.first) { last = k.second; shelf = t == "shelf" || t == "shelves"; }
    }
    if (blocker) return kHintBlocker;
    if (planter && last != kHintSeat) return kHintNature;
    if (rock && shelf) return kHintNature;        // a rock's shelf (a stone table or counter is still furniture)
    return last;
}

// Whether a name says it's ground (floor, ground, terrain, a path, a shore: "GrassGround" too), not plant cards or
// tufts standing on it ("cWeedFloor_Card").
inline bool walkNameIsGround(const std::string& name) {
    const std::vector<std::string> words = walkNameWords(name);
    if (walkNameIsPlant(words) && walkNameHasWord(words, {"card", "blade", "clump", "tuft", "sprig", "patch"})) return false;
    const WalkHint h = walkNameHint(name);                       // (a floor lamp is a lamp, a deck chair a chair)
    if (h != kHintFloor && h != kHintNone && h != kHintNature) return false;
    return walkNameHasWord(words, {"floor", "ground", "terrain", "path", "shore", "beach", "seabed", "seafloor", "sand", "gravel",
                                   "dirt", "lawn", "deck", "terrace", "stairs", "steps", "rug", "carpet", "pavement", "sidewalk"});
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
    bool solidShape = false;            // a closed convex shape (one of the home's own box, ball or capsule colliders): with
                                        // buildWalkGrid's homeFloor it fills the cells whose middle it covers, bottom to top
    WalkHint hint = kHintNone;          // what its name says it is (the generator's; the bake: a seat's or a table's top is never floor)
    std::string name;                   // the mesh's (the generator names its collision after it)
};

// Cut-out geometry that grew (grass, ferns, a bush, a hedge, leaves), not built or named as furniture (a fence, a
// railing, a lattice bench or table, a wall): never floor, walked through where it's low on the ground, passed through
// by a teleport arc. Built cut-outs block as solid things do.
inline bool walkSourceGrown(const WalkSource& s) {
    return s.cutout && !walkNameIsStructure(s.name) && s.hint != kHintSeat && s.hint != kHintFurniture && s.hint != kHintWall &&
           s.hint != kHintBlocker;
}

// Whether one of the home's colliders is a drawn mesh's own collision: on its entity (the same frame, `world`) and, a
// mesh, the mesh's own triangles (Meta's physics mesh is read as the drawn one: the same bounds, within the 2.5 cm a
// simplified copy may have moved); a box, ball or capsule inside the mesh's bounds (5 cm slack).
inline bool walkColliderOfMesh(const WorldCollider& c, const float* world, const std::vector<float>& pos) {
    if (pos.size() < 9) return false;
    float scale = 1e-6f;
    for (int k = 0; k < 16; ++k) if (std::fabs(c.world[k] - world[k]) > 1e-4f * (1 + std::fabs(world[k]))) return false;
    for (int col = 0; col < 3; ++col) scale = std::max(scale, std::sqrt(world[col * 4] * world[col * 4] + world[col * 4 + 1] * world[col * 4 + 1] + world[col * 4 + 2] * world[col * 4 + 2]));
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t v = 0; v + 2 < pos.size(); v += 3) for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], pos[v + k]); hi[k] = std::max(hi[k], pos[v + k]); }
    if (c.kind == WorldCollider::Mesh) {
        if (c.positions.size() < 9) return false;
        const float tol = 0.03f / scale;
        float clo[3] = {1e30f, 1e30f, 1e30f}, chi[3] = {-1e30f, -1e30f, -1e30f};
        for (size_t v = 0; v + 2 < c.positions.size(); v += 3) for (int k = 0; k < 3; ++k) { clo[k] = std::min(clo[k], c.positions[v + k]); chi[k] = std::max(chi[k], c.positions[v + k]); }
        for (int k = 0; k < 3; ++k) if (std::fabs(clo[k] - lo[k]) > tol || std::fabs(chi[k] - hi[k]) > tol) return false;
        return true;
    }
    // a shape: its corners (a box's, or the box round a ball or capsule) in the mesh's frame, within its bounds
    const float r = c.radius, hy = c.kind == WorldCollider::Capsule ? c.length * 0.5f + r : r;
    const float he[3] = {c.kind == WorldCollider::Box ? c.half[0] : r, c.kind == WorldCollider::Box ? c.half[1] : hy, c.kind == WorldCollider::Box ? c.half[2] : r};
    const float tol = 0.05f / scale;
    for (int k = 0; k < 3; ++k) if (-he[k] < lo[k] - tol || he[k] > hi[k] + tol) return false;
    return true;
}

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
        // line along a wall at an angle, not on the cells' staircase. The fall is at least kMinFall per metre: in a gap
        // narrower than a cell or two the distances (to its sides) hardly change along it, and carried on as they are
        // they'd say the floor runs on past the gap's end, a knob of floor poking into the furniture.
        constexpr float kMinFall = 0.4f;          // a wall more than ~24 degrees off the direction still carries on as it is
        auto outside = [&](int ci, int cj) {
            static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
            float sum = 0;
            int n = 0;
            for (const auto& d : dirs) {
                float a, b;
                if (!distAt(ci + d[0], cj + d[1], a)) continue;
                const float stepLen = d[0] && d[1] ? p.cell * 1.41421f : p.cell;
                const float est = distAt(ci + 2 * d[0], cj + 2 * d[1], b) && b > a + 1e-4f ? a - std::max(b - a, kMinFall * stepLen) : a - stepLen;
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

    // Where to stand up from a seat at pos (its sitting point) facing yaw (0: -Z): the nearest good spot, in front first
    // and then round to the sides, behind last (at 0.45 to 1.3 m: off the seat, not across the room). Good: on the area
    // at the floor the seat stands on, with the way to it on the area from where the seat ends (its own cushion may
    // reach 0.85 m out: a deep sofa, a clam chair) on: not over a coffee table or round a corner. out: the spot on the
    // floor. False: there's none (the seat is walled in).
    bool standUpSpot(const float pos[3], float yaw, float out[3]) const {
        const float floorY = pos[1] - 0.45f;                               // (a seat is about knee height)
        static const float kAngles[] = {0, 20, -20, 40, -40, 65, -65, 90, -90, 120, -120, 150, -150, 180};   // degrees off its front
        static const float kOut[] = {0.45f, 0.6f, 0.75f, 0.9f, 1.1f, 1.3f};
        const float c = std::cos(yaw), s = std::sin(yaw);
        for (float deg : kAngles) {
            const float a = deg * 3.14159265f / 180;
            // the seat's front (-Z, turned by yaw) turned by a
            const float fx = -s, fz = -c, dx = fx * std::cos(a) - fz * std::sin(a), dz = fx * std::sin(a) + fz * std::cos(a);
            for (float d : kOut) {
                const float x = pos[0] + dx * d, z = pos[2] + dz * d;
                float h;
                if (!floorAt(x, z, floorY + p.step, p.step, p.step + 0.3f, h) || !walkable(x, z, h)) continue;
                bool clear = true, onArea = false;                         // the way there: off the seat, then all on the area
                for (float t = 0.25f; t < d && clear; t += 0.1f) {
                    if (walkable(pos[0] + dx * t, pos[2] + dz * t, h)) onArea = true;
                    else if (onArea || t > 0.85f) clear = false;           // (off the area again: a table, a corner)
                }
                if (!clear) continue;
                out[0] = x; out[1] = h; out[2] = z;
                return true;
            }
        }
        return false;
    }

    // Where standing up from a seat puts you: its own stand-up spot (its exit, turned by its yaw; half a metre in front
    // when it names none) when that's on the area, else the nearest good spot round the seat (standUpSpot). Never
    // somewhere you can't walk. out: the spot on the floor. ownSpot: the seat's own spot was good. False: nowhere.
    bool seatExit(const WorldPoint& seat, float out[3], bool* ownSpot = nullptr) const {
        if (ownSpot) *ownSpot = false;
        const float c = std::cos(seat.yaw), s = std::sin(seat.yaw);
        float ex = seat.exit[0], ez = seat.exit[2];
        if (std::hypot(ex, ez) < 0.05f) { ex = 0; ez = -0.5f; }
        const float x = seat.pos[0] + c * ex + s * ez, z = seat.pos[2] - s * ex + c * ez;
        float h;
        if (floorAt(x, z, seat.pos[1], 0.3f, 2.0f, h) && walkable(x, z, h)) {
            out[0] = x; out[1] = h; out[2] = z;
            if (ownSpot) *ownSpot = true;
            return true;
        }
        return standUpSpot(seat.pos, seat.yaw, out);
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
        // The cells whose floors can reach into a quad: its 4 corners, then the ring of 12 round them (field() reads
        // the 3 x 3 cells round the nearest centre, so the area can run into a quad none of whose corners is floor;
        // without the ring that quad went undrawn and the floor beside it ended on a straight cell side).
        static const int kReach[16][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1},
                                          {-1, -1}, {0, -1}, {1, -1}, {2, -1}, {2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {-1, 2}, {-1, 1}, {-1, 0}};
        for (int z = 0; z + 1 < nz; drawRuns(z), ++z)
            for (int x = 0; x + 1 < nx; ++x)
                for (int k0 = 0; k0 < 16; ++k0) {
                    const int sx = x + kReach[k0][0], sz = z + kReach[k0][1];
                    if (!inside(sx, sz)) continue;
                    for (const WalkSpan* s0 = begin(sx, sz); s0 != end(sx, sz); ++s0) {
                        if (!(s0->flags & (kWalkOk | kWalkNear))) continue;
                        bool earlier = false;                      // this level is drawn from an earlier cell
                        for (int j = 0; j < k0 && !earlier; ++j) earlier = walkNear(x + kReach[j][0], z + kReach[j][1], s0->hi, p.step) != nullptr;
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
    static constexpr float kWalkThroughFoliage = 1.0f;   // cut-out foliage this tall on the ground is walked through (m)
    // Floor painted by hand (HomeWorld::walkPaint) goes through see-through plants: cut-out geometry in a painted
    // cell, from a little under its lowest paint to a body's height over its highest, isn't there to walk into.
    // paintLevels: per grid cell (not the band's), the painted levels (lo > hi: none). leafTop: per band cell, the
    // highest such plant top left out (a dab that touched it is on the ground under it).
    static constexpr float kPlantBelow = 0.3f, kPlantAbove = 2.0f;
    const std::vector<std::array<float, 2>>* paintLevels = nullptr;
    std::vector<float> leafTop;
    // Per band cell, the top of the plant pieces add() let into the ground (none: -1e30): a card's upper part
    // rasterized after its lower part went in starts there, not on the ground (dropRootedFoliage).
    std::vector<float> rootedLeafTop;

    int alloc(const BSpan& s) {
        if (freeList >= 0) { const int i = freeList; freeList = pool[i].next; pool[i] = s; return i; }
        pool.push_back(s);
        return static_cast<int>(pool.size()) - 1;
    }
    // Grass and low plants rooted on the ground are walked through (kWalkThroughFoliage), all of them: the pieces
    // add() couldn't merge into the ground (a card's upper half, rasterized after its lower half went into it) too.
    // A run of cut-out pieces starting at most 30 cm over a solid top, each at most 30 cm over the last, whose top is
    // at most kWalkThroughFoliage over it, goes; a taller one (a hedge, a tree) stays. (Not pieces floating higher: a
    // pond's or a hill's cut-out fringe over the ground under it isn't a plant to walk through.)
    void dropRootedFoliage() {
        constexpr float kGap = 0.3f;
        for (size_t cell = 0; cell < head.size(); ++cell) {
            int ground = -1, prev = -1;
            for (int i = head[cell]; i >= 0;) {
                if (!(pool[i].cut & kWalkCutAll)) { ground = prev = i; i = pool[i].next; continue; }
                // (from the ground, or the top of a plant's part already let into it: the same plant going on up)
                const float base = ground >= 0 ? pool[ground].hi : 0;
                const float rooted = rootedLeafTop.empty() ? -1e30f : rootedLeafTop[cell];
                const float from = rooted > base && rooted <= base + kWalkThroughFoliage ? rooted : base;
                if (ground >= 0 && pool[i].lo - from <= kGap) {
                    float top = pool[i].hi;
                    int last = i, j = pool[i].next;
                    while (j >= 0 && (pool[j].cut & kWalkCutAll) && pool[j].lo - pool[last].hi <= kGap) { top = std::max(top, pool[j].hi); last = j; j = pool[j].next; }
                    if (top - pool[ground].hi <= kWalkThroughFoliage) {
                        if (prev < 0) head[cell] = j; else pool[prev].next = j;
                        for (int q = i; q != j;) { const int n = pool[q].next; pool[q].next = freeList; freeList = q; q = n; }
                        i = j;
                        continue;
                    }
                }
                prev = i;
                i = pool[i].next;
            }
        }
    }
    void add(int cell, float lo, float hi, uint8_t flags, int tag, uint8_t cut = 0) {
        if ((cut & kWalkCutAll) && paintLevels) {
            const std::array<float, 2>& lv = (*paintLevels)[static_cast<size_t>(z0) * nx + cell];
            if (lv[0] <= lv[1] && lo <= lv[1] + kPlantAbove && hi >= lv[0] - kPlantBelow) {
                if (leafTop.empty()) leafTop.assign(head.size(), -1e30f);
                leafTop[cell] = std::max(leafTop[cell], hi);
                return;
            }
        }
        BSpan s{lo, hi, -1, tag, flags, cut};
        int prev = -1, cur = head[cell];
        while (cur >= 0) {
            BSpan& c = pool[cur];
            if (c.lo > s.hi + kMergeGap) break;
            if (c.hi < s.lo - kMergeGap) { prev = cur; cur = c.next; continue; }
            constexpr uint8_t top = kWalkUp | kWalkFloorRole | kWalkFurniture;
            const bool sCut = s.cut & kWalkCutAll, cCut = c.cut & kWalkCutAll;
            bool leafDropped = false;                                      // (grass on the ground: the ground stays as it is)
            if (sCut != cCut) {
                // grass, ferns, flowers, a low bush, rooted on the ground and at most kWalkThroughFoliage tall: walked
                // through, nothing of them in the way (a hedge or a tree is taller: it still blocks)
                const BSpan& leaf = sCut ? s : c;
                const BSpan& ground = sCut ? c : s;
                if (leaf.lo >= ground.hi - 0.3f && leaf.hi - ground.hi <= kWalkThroughFoliage) {
                    if (rootedLeafTop.empty()) rootedLeafTop.assign(head.size(), -1e30f);
                    rootedLeafTop[cell] = std::max(rootedLeafTop[cell], leaf.hi);
                    if (sCut) { s.hi = c.hi; s.flags = c.flags; s.tag = c.tag; s.cut = c.cut; }
                    leafDropped = true;
                }
            }
            if (leafDropped) {
            } else if (sCut != cCut && (sCut ? c.hi >= s.hi - 0.15f : s.hi >= c.hi - 0.15f)) {
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
            if (!leafDropped || !cCut) s.hi = std::max(s.hi, c.hi);     // (a leaf merged in leaves the ground's top)
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

// The narrowest a set of points is seen from above (XZ): their convex hull's width, the least over its sides' directions.
inline float footprintWidth(const std::vector<float>& wp) {
    using P2 = std::array<float, 2>;
    std::vector<P2> p;
    for (size_t v = 0; v + 2 < wp.size(); v += 3) p.push_back({wp[v], wp[v + 2]});
    std::sort(p.begin(), p.end());
    p.erase(std::unique(p.begin(), p.end()), p.end());
    if (p.size() < 3) return 0;
    auto cross = [](const P2& o, const P2& a, const P2& b) { return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]); };
    std::vector<P2> h(2 * p.size());                                  // (Andrew's monotone chain)
    size_t k = 0;
    for (size_t i = 0; i < p.size(); ++i) { while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k; h[k++] = p[i]; }
    for (size_t i = p.size() - 1, lower = k + 1; i-- > 0;) { while (k >= lower && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k; h[k++] = p[i]; }
    h.resize(k - 1);
    if (h.size() < 3) return 0;
    float best = 1e30f;
    for (size_t i = 0; i < h.size(); ++i) {
        const P2& a = h[i]; const P2& b = h[(i + 1) % h.size()];
        const float L = std::hypot(b[0] - a[0], b[1] - a[1]);
        if (L < 1e-9f) continue;
        float w = 0;
        for (const P2& q : h) w = std::max(w, std::fabs(cross(a, b, q)) / L);
        best = std::min(best, w);
    }
    return best;
}

// The sources' own triangles, asked exactly where a floor ends (a home walked on its own collision: buildWalkGrid's
// homeFloor). The cells only say that something is somewhere in a cell: a wall's side clipping a cell's corner takes
// the whole cell, a floor reaching into one gives it all. These answer at a point and along a line instead: is there a
// floor to stand on here, and how far from a floor's middle the body first meets something or the floor runs out.
struct SurfaceProbe {
    enum : uint8_t { kFloor = 1, kBlocks = 2, kWall = 4 };   // stood on; in the way over a floor; in the way at any height (Wall, Object)
    struct Tri { float v[9]; uint8_t what; };
    float ox = 0, oz = 0, size = 1;     // the buckets: size x size m squares from (ox, oz)
    int nx = 0, nz = 0;
    float step = 0.4f, headroom = 1.6f;
    std::vector<Tri> tris;
    std::vector<std::vector<uint32_t>> bucket, floorBucket;   // per bucket: what blocks; what's stood on
    mutable std::vector<uint32_t> seen;  // (the query that last looked at each triangle: one look per query)
    mutable uint32_t query = 0;

    void init(float x0, float z0, float x1, float z1, float bucketSize, float stepIn, float headroomIn) {
        ox = x0; oz = z0; size = bucketSize; step = stepIn; headroom = headroomIn;
        nx = std::max(1, static_cast<int>(std::ceil((x1 - x0) / size))); nz = std::max(1, static_cast<int>(std::ceil((z1 - z0) / size)));
        bucket.assign(static_cast<size_t>(nx) * nz, {});
        floorBucket.assign(static_cast<size_t>(nx) * nz, {});
    }
    void add(const float* a, const float* b, const float* c, uint8_t what) {
        if (!what) return;
        const int x0 = std::max(0, static_cast<int>(std::floor((std::min({a[0], b[0], c[0]}) - ox) / size)));
        const int x1 = std::min(nx - 1, static_cast<int>(std::floor((std::max({a[0], b[0], c[0]}) - ox) / size)));
        const int z0 = std::max(0, static_cast<int>(std::floor((std::min({a[2], b[2], c[2]}) - oz) / size)));
        const int z1 = std::min(nz - 1, static_cast<int>(std::floor((std::max({a[2], b[2], c[2]}) - oz) / size)));
        if (x0 > x1 || z0 > z1) return;
        const uint32_t id = static_cast<uint32_t>(tris.size());
        Tri t;
        std::copy_n(a, 3, t.v); std::copy_n(b, 3, t.v + 3); std::copy_n(c, 3, t.v + 6);
        t.what = what;
        tris.push_back(t);
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                if (what & (kBlocks | kWall)) bucket[static_cast<size_t>(z) * nx + x].push_back(id);
                if (what & kFloor) floorBucket[static_cast<size_t>(z) * nx + x].push_back(id);
            }
    }
    // each triangle that blocks in the buckets over [x0, x1] x [z0, z1], once
    template <typename F> void each(float x0, float z0, float x1, float z1, F&& fn) const {
        if (seen.size() != tris.size()) seen.assign(tris.size(), 0);
        if (++query == 0) { std::fill(seen.begin(), seen.end(), 0); query = 1; }
        const int bx0 = std::max(0, static_cast<int>(std::floor((x0 - ox) / size))), bx1 = std::min(nx - 1, static_cast<int>(std::floor((x1 - ox) / size)));
        const int bz0 = std::max(0, static_cast<int>(std::floor((z0 - oz) / size))), bz1 = std::min(nz - 1, static_cast<int>(std::floor((z1 - oz) / size)));
        for (int z = bz0; z <= bz1; ++z)
            for (int x = bx0; x <= bx1; ++x)
                for (uint32_t i : bucket[static_cast<size_t>(z) * nx + x]) {
                    if (seen[i] == query) continue;
                    seen[i] = query;
                    fn(tris[i]);
                }
    }
    // The floor under (x, z) within a step of h, nearest h (a ramp's height where it is): false when there's none.
    bool floorAt(float x, float z, float h, float& y) const {
        bool found = false;
        float best = step + 1e-4f;
        const int bx = static_cast<int>(std::floor((x - ox) / size)), bz = static_cast<int>(std::floor((z - oz) / size));
        if (bx < 0 || bz < 0 || bx >= nx || bz >= nz) return false;
        for (uint32_t i : floorBucket[static_cast<size_t>(bz) * nx + bx]) {   // (a point is in one bucket: each triangle once)
            const Tri& t = tris[i];
            const float* a = t.v; const float* b = t.v + 3; const float* c = t.v + 6;
            const float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
            if (std::fabs(d) < 1e-12f) continue;
            const float u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d;
            const float w = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d;
            if (u < -1e-5f || w < -1e-5f || u + w > 1 + 1e-5f) continue;
            const float ty = u * a[1] + w * b[1] + (1 - u - w) * c[1];
            if (std::fabs(ty - h) < best) { best = std::fabs(ty - h); y = ty; found = true; }
        }
        return found;
    }
    // How far from (x, z) along (ux, uz) (unit, XZ) a body standing on the floor at h first meets something: a wall
    // anywhere from a step under its feet to its head, anything else over the floor (past what the cells take as part
    // of it, Raster::kMergeGap) up to its head. Each triangle meets the upright plane through the line in a segment;
    // its part inside that band and within `len` is the hit. len + 1: nothing within len.
    float blockAlong(float x, float z, float ux, float uz, float len, float h) const {
        const float nX = -uz, nZ = ux;                               // the plane's normal
        float first = len + 1;
        each(std::min(x, x + ux * len), std::min(z, z + uz * len), std::max(x, x + ux * len), std::max(z, z + uz * len), [&](const Tri& t) {
            const float ylo = h + (t.what & kWall ? -step : Raster::kMergeGap), yhi = h + headroom;
            float sd[3], pt[3][2];                                   // each corner: its side of the plane; (along, y)
            for (int k = 0; k < 3; ++k) {
                const float* p = t.v + k * 3;
                sd[k] = (p[0] - x) * nX + (p[2] - z) * nZ;
                pt[k][0] = (p[0] - x) * ux + (p[2] - z) * uz;
                pt[k][1] = p[1];
            }
            if ((sd[0] > 0 && sd[1] > 0 && sd[2] > 0) || (sd[0] < 0 && sd[1] < 0 && sd[2] < 0)) return;
            // where it meets the plane: a segment (a triangle lying in it: each of its edges)
            float seg[6][2];
            int n = 0;
            for (int k = 0; k < 3; ++k) {
                const int j = (k + 1) % 3;
                if (sd[k] == 0) { seg[n][0] = pt[k][0]; seg[n][1] = pt[k][1]; ++n; }
                if ((sd[k] < 0 && sd[j] > 0) || (sd[k] > 0 && sd[j] < 0)) {
                    const float s = sd[k] / (sd[k] - sd[j]);
                    seg[n][0] = pt[k][0] + (pt[j][0] - pt[k][0]) * s; seg[n][1] = pt[k][1] + (pt[j][1] - pt[k][1]) * s; ++n;
                }
            }
            for (int a = 0; a < n; ++a)
                for (int b = a; b < n; ++b) {
                    // the piece of a -> b inside along [0, len], y [ylo, yhi] (Liang-Barsky); its nearest along
                    float t0 = 0, t1 = 1;
                    const float p0[2] = {seg[a][0], seg[a][1]}, d[2] = {seg[b][0] - seg[a][0], seg[b][1] - seg[a][1]};
                    auto clip = [&](float p, float q) {              // keep p * t <= q
                        if (std::fabs(p) < 1e-12f) return q >= 0;
                        const float r = q / p;
                        if (p < 0) { if (r > t1) return false; t0 = std::max(t0, r); } else { if (r < t0) return false; t1 = std::min(t1, r); }
                        return true;
                    };
                    if (!clip(-d[0], p0[0]) || !clip(d[0], len - p0[0]) || !clip(-d[1], p0[1] - ylo) || !clip(d[1], yhi - p0[1]) || t0 > t1) continue;
                    first = std::min(first, std::min(p0[0] + d[0] * t0, p0[0] + d[0] * t1));
                }
        });
        return first;
    }
    // Where the floor at h under (x, z) ends along (ux, uz), within len: the body meets something, or the floor runs
    // out (no floor a step or less from the last one: a drop, a hole, its edge). Followed in 1 cm steps, the end found
    // to a millimetre. len + 1: it goes on past len.
    float edgeAlong(float x, float z, float ux, float uz, float len, float h) const {
        const float block = blockAlong(x, z, ux, uz, len, h);
        const float upTo = std::min(len, block);
        constexpr float kStep = 0.01f;
        float y = h, prev = 0;
        for (float t = kStep; t <= upTo + kStep * 0.5f; t += kStep) {
            const float at = std::min(t, upTo);
            float yt;
            if (floorAt(x + ux * at, z + uz * at, y, yt)) { y = yt; prev = at; continue; }
            float a = prev, b = at;
            for (int i = 0; i < 6; ++i) { const float m = (a + b) * 0.5f; float ym; (floorAt(x + ux * m, z + uz * m, y, ym) ? a : b) = m; }
            return b;
        }
        return block;
    }
};

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
// A stroke of painted floor against the grid: its XZ extent (a round dab at least 1 cm, at most 20 m across), whether a
// cell's middle is inside it (a dab smaller than a cell still paints the cell it's in), and each cell inside it.
inline void strokeExtent(const WalkStroke& k, float* lo, float* hi) {
    if (k.shape == WalkStroke::Round) {
        const float r = std::clamp(k.radius, 0.01f, 20.0f);
        lo[0] = k.pos[0] - r; lo[1] = k.pos[2] - r; hi[0] = k.pos[0] + r; hi[1] = k.pos[2] + r;
        return;
    }
    k.extent(lo, hi);
}
inline bool strokeCovers(const WalkStroke& k, float x, float z, float cell) {
    if (k.shape == WalkStroke::Round) {
        const float r = std::clamp(k.radius, 0.01f, 20.0f), dx = x - k.pos[0], dz = z - k.pos[2];
        return dx * dx + dz * dz <= std::max(r * r, 0.25f * cell * cell);
    }
    return k.covers(x, z, 0.5f * cell);
}
template <typename F> void forStrokeCells(const WalkStroke& k, float ox, float oz, float cell, int nx, int nz, F&& fn) {
    float lo[2], hi[2];
    strokeExtent(k, lo, hi);
    // (clamped as floats first: a stroke far past the grid mustn't overflow an int)
    auto at = [cell](float v, float o, int n) { return static_cast<int>(std::clamp(std::floor((v - o) / cell), -1.0f, static_cast<float>(n))); };
    const int x0 = std::max(0, at(lo[0], ox, nx)), x1 = std::min(nx - 1, at(hi[0], ox, nx));
    const int z0 = std::max(0, at(lo[1], oz, nz)), z1 = std::min(nz - 1, at(hi[1], oz, nz));
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
            if (strokeCovers(k, ox + (x + 0.5f) * cell, oz + (z + 0.5f) * cell, cell)) fn(x, z);
}
// How far under where a stroke touched a see-through plant the ground it's on can be (a hedge's height)
constexpr float kPlantDrop = 2.5f;

// homeFloor: the sources are the home's own collision (Meta's colliders, HomeWorld::hasHomeCollision), and the area is
// exactly its floor: nothing made up (no seams bridged, `gap` unused) and nothing of it left out but what its own
// geometry blocks (the body `radius` doesn't part or trim it, no top is peeled off by its shape: every floor stood on is
// in the area, its edge right where the floor ends or the body meets something: found on the triangles themselves, not
// the cells). The step, headroom, steepest floor and the start's reach still decide what's walked to, and a named
// seat's or table's top is still never walked on (4b's named rule: seats and tables never are). P is kept as given
// (Studio's settings), so the area reads back with them.
inline std::shared_ptr<WalkGrid> buildWalkGrid(const std::vector<WalkSource>& sources, const std::vector<std::array<float, 3>>& seeds,
                                               const WalkParams& P, WalkReport* report = nullptr, const float* anchorIn = nullptr,
                                               const std::vector<WalkStroke>* paint = nullptr, bool homeFloor = false) {
    using namespace walkgrid_detail;
    const auto t0 = std::chrono::steady_clock::now();
    WalkReport rep;
    auto g = std::make_shared<WalkGrid>();
    g->p = P;
    const float cs = P.cell;
    const float gap = homeFloor ? 0.0f : P.gap, radius = homeFloor ? 0.0f : P.radius;   // (the home's own floor: see above)
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
    if (paint)                                                         // so is floor painted where nothing is
        for (const WalkStroke& k : *paint) {
            if (k.erase) continue;
            float elo[2], ehi[2];
            strokeExtent(k, elo, ehi);
            lo[0] = std::min(lo[0], elo[0]); hi[0] = std::max(hi[0], ehi[0]);
            lo[1] = std::min(lo[1], elo[1]); hi[1] = std::max(hi[1], ehi[1]);
        }
    // never past the reach (a far spawn, a shape's corner clicked at the horizon, a damaged file: not a grid of millions)
    lo[0] = std::max(lo[0], anchor[0] - P.reach); hi[0] = std::min(hi[0], anchor[0] + P.reach);
    lo[1] = std::max(lo[1], anchor[2] - P.reach); hi[1] = std::min(hi[1], anchor[2] + P.reach);
    if (lo[0] > hi[0] || lo[1] > hi[1]) { if (report) *report = rep; return g; }
    const float R_ox = std::floor(lo[0] / cs) * cs - cs, R_oz = std::floor(lo[1] / cs) * cs - cs;
    const int nx = static_cast<int>(std::ceil((hi[0] - R_ox) / cs)) + 2, nz = static_cast<int>(std::ceil((hi[1] - R_oz) / cs)) + 2;
    const size_t ncell = static_cast<size_t>(nx) * nz;
    // 1. Rasterize: each triangle is told what it is (floor, wall) and which rows it covers; the rows go in bands
    //    of about equal work (a home's geometry is anything but even), rasterized in parallel.
    const float cosSlope = std::cos(P.maxSlope * 3.14159265f / 180);
    struct BTri { uint32_t src, tri; int z0, z1; uint8_t flags, cut; };
    std::vector<BTri> tris;
    std::vector<double> rowWork(nz, 0);
    // The home's own boxes, balls and capsules 1.5 cells across or more go in by the cells' middles (below): a cell has
    // one where its middle is inside it, not wherever its side passes (a wall clipping a cell's corner isn't in the
    // way of the floor at its middle). Thinner ones go in as triangles: whole, a wall can't fall between the middles.
    std::vector<size_t> shapes;
    for (size_t s = 0; s < sources.size(); ++s) {
        const WalkSource& src = sources[s];
        const std::vector<float>& wp = wpos[s];
        if (wp.empty()) continue;
        if (box[s][2] < R_ox || box[s][3] < R_oz || box[s][0] > R_ox + nx * cs || box[s][1] > R_oz + nz * cs) continue;   // outside it all
        if (homeFloor && src.solidShape && footprintWidth(wp) >= 1.5f * cs) { shapes.push_back(s); continue; }
        // cut-out: never floor; all of it cut-out (low plants walked through, an arc passes through leaves) unless
        // it's built or named as furniture (a fence, a railing, a lattice bench or table, a wall): that blocks as a
        // solid thing does
        const uint8_t cut = !src.cutout ? 0 : walkSourceGrown(src) ? kWalkCutTop | kWalkCutAll : kWalkCutTop;
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
            else if (src.role == WorldCollider::Floor || src.role == WorldCollider::Walkable || src.role == WorldCollider::Navmesh) { if (up < 0.5f) continue; fl = kWalkUp | kWalkFloorRole; }   // never blocks
            else if (!src.noFloor && !src.cutout && up >= cosSlope) fl = kWalkUp;   // (foliage is never floor)
            tris.push_back({static_cast<uint32_t>(s), static_cast<uint32_t>(t), z0, z1, fl, cut});
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
    // painted floor's cells and levels (see-through plants there are walked through)
    std::vector<std::array<float, 2>> paintLevels;
    if (paint)
        for (const WalkStroke& k : *paint) {
            if (k.erase) continue;
            if (paintLevels.empty()) paintLevels.assign(ncell, {1e30f, -1e30f});
            forStrokeCells(k, R_ox, R_oz, cs, nx, nz, [&](int x, int z) {
                std::array<float, 2>& lv = paintLevels[static_cast<size_t>(z) * nx + x];
                lv[0] = std::min(lv[0], k.pos[1]); lv[1] = std::max(lv[1], k.pos[1]);
            });
        }
    std::vector<Raster> band(nb);
    std::vector<int> bandOfRow(nz);
    for (int b = 0; b < nb; ++b) {
        Raster& B = band[b];
        B.cs = cs; B.ox = R_ox; B.oz = R_oz; B.z0 = bandStart[b]; B.nx = nx; B.nz = bandStart[b + 1] - bandStart[b];
        B.head.assign(static_cast<size_t>(nx) * B.nz, -1);
        if (!paintLevels.empty()) B.paintLevels = &paintLevels;
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
        B.dropRootedFoliage();
        std::vector<uint32_t>().swap(bandTris[b]);
    });
    // what a triangle is to the floor: stood on (kWalkUp), in the way at any height (kWalkWall), or neither
    auto triFlags = [&](const WalkSource& src, float up) -> uint8_t {
        if (src.role == WorldCollider::Wall || src.role == WorldCollider::Object) return kWalkWall;
        if (src.role == WorldCollider::Floor || src.role == WorldCollider::Walkable || src.role == WorldCollider::Navmesh) return up >= 0.5f ? kWalkUp | kWalkFloorRole : 0;
        return !src.noFloor && !src.cutout && up >= cosSlope ? kWalkUp : 0;
    };
    auto upOf = [](const float* a, const float* b, const float* c) {   // how much a triangle faces up (either way), 0..1
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float L = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        return L < 1e-12f ? 0.0f : std::fabs(n[1]) / L;
    };
    // the shapes by the cells' middles: from where the line down through it enters to where it leaves; its top is
    // stood on when the face there is
    for (size_t s : shapes) {
        const WalkSource& src = sources[s];
        const float* wp = wpos[s].data();
        const int x0 = std::max(0, static_cast<int>(std::floor((box[s][0] - R_ox) / cs - 0.5f))), x1 = std::min(nx - 1, static_cast<int>(std::ceil((box[s][2] - R_ox) / cs - 0.5f)));
        const int z0 = std::max(0, static_cast<int>(std::floor((box[s][1] - R_oz) / cs - 0.5f))), z1 = std::min(nz - 1, static_cast<int>(std::ceil((box[s][3] - R_oz) / cs - 0.5f)));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const float px = R_ox + (x + 0.5f) * cs, pz = R_oz + (z + 0.5f) * cs;
                float lo = 1e30f, hi = -1e30f, topUp = 0;
                for (size_t t = 0; t + 2 < src.indices; t += 3) {
                    if (src.idx[t] >= src.verts || src.idx[t + 1] >= src.verts || src.idx[t + 2] >= src.verts) continue;
                    const float* a = &wp[src.idx[t] * 3]; const float* b = &wp[src.idx[t + 1] * 3]; const float* c = &wp[src.idx[t + 2] * 3];
                    const float d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
                    if (std::fabs(d) < 1e-12f) continue;               // (upright: seen edge-on from above)
                    const float u = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / d;
                    const float w = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / d;
                    if (u < -1e-5f || w < -1e-5f || u + w > 1 + 1e-5f) continue;
                    const float y = u * a[1] + w * b[1] + (1 - u - w) * c[1];
                    lo = std::min(lo, y);
                    if (y > hi) { hi = y; topUp = upOf(a, b, c); }
                }
                if (lo <= hi) addAt(static_cast<size_t>(z) * nx + x, lo, hi, triFlags(src, topUp), src.tag);
            }
    }
    // The home's own floor: its triangles, kept to find exactly where it ends (what stands on it blocks; a Floor-role
    // surface never does)
    SurfaceProbe probe;
    if (homeFloor) {
        probe.init(R_ox, R_oz, R_ox + nx * cs, R_oz + nz * cs, 4 * cs, P.step, P.headroom);
        auto put = [&](const WalkSource& src, const float* a, const float* b, const float* c, uint8_t flags, uint8_t cut) {
            const bool floorRole = src.role == WorldCollider::Floor || src.role == WorldCollider::Walkable || src.role == WorldCollider::Navmesh;
            const uint8_t blocks = flags & kWalkWall ? SurfaceProbe::kWall : !floorRole && !(cut & kWalkCutAll) ? SurfaceProbe::kBlocks : 0;
            probe.add(a, b, c, static_cast<uint8_t>((flags & kWalkUp ? SurfaceProbe::kFloor : 0) | blocks));
        };
        for (const BTri& bt : tris) {
            const WalkSource& src = sources[bt.src];
            const float* wp = wpos[bt.src].data();
            put(src, &wp[src.idx[bt.tri] * 3], &wp[src.idx[bt.tri + 1] * 3], &wp[src.idx[bt.tri + 2] * 3], bt.flags, bt.cut);
        }
        for (size_t s : shapes) {
            const WalkSource& src = sources[s];
            const float* wp = wpos[s].data();
            for (size_t t = 0; t + 2 < src.indices; t += 3) {
                if (src.idx[t] >= src.verts || src.idx[t + 1] >= src.verts || src.idx[t + 2] >= src.verts) continue;
                const float* a = &wp[src.idx[t] * 3]; const float* b = &wp[src.idx[t + 1] * 3]; const float* c = &wp[src.idx[t + 2] * 3];
                put(src, a, b, c, triFlags(src, upOf(a, b, c)), 0);
            }
        }
    }
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
    // The floor brush's dabs (HomeWorld::walkPaint), in order, onto the surface under each: a painted top is a floor
    // the way a drawn floor is (stood on at any slope, kept even on furniture or where nobody starts, right up to its
    // edge); an erased one is no floor at all (nor bridged over). A dab takes the top it touched and the tops within
    // its radius joined to that one a step or less apart (a slope, a stair), not a table beside it or a floor below.
    std::vector<uint8_t> painted;                                     // per span: 0 untouched, 1 painted, 2 erased
    auto applyPaint = [&] {
        painted.assign(cs_.size(), 0);
        if (!paint) return;
        std::vector<std::pair<uint32_t, int32_t>> stack;              // (span, its cell's x + z * nx)
        std::vector<uint32_t> visit(cs_.size(), 0);                   // the dab (1-based) that last reached it
        uint32_t dab = 0;
        for (const WalkStroke& k : *paint) {
            ++dab;
            const int cx = static_cast<int>(std::floor((k.pos[0] - R_ox) / cs)), cz = static_cast<int>(std::floor((k.pos[2] - R_oz) / cs));
            auto inDab = [&](int x, int z) {
                if (x < 0 || z < 0 || x >= nx || z >= nz) return false;
                return strokeCovers(k, R_ox + (x + 0.5f) * cs, R_oz + (z + 0.5f) * cs, cs);
            };
            // the top it touched: nearest its height in the cells round where it touched (a rectangle's or a
            // polygon's: inside it, as the corner it was started from is on its edge; a sharp corner, further in)
            uint32_t first = UINT32_MAX;
            int32_t firstCell = -1;
            float bd = 0.3f;
            const bool round = k.shape == WalkStroke::Round;
            for (int w = 1; w <= (round ? 1 : 3) && first == UINT32_MAX; ++w)
                for (int z = cz - w; z <= cz + w; ++z)
                    for (int x = cx - w; x <= cx + w; ++x) {
                        if (x < 0 || z < 0 || x >= nx || z >= nz || (!round && !inDab(x, z))) continue;
                        const size_t cell = static_cast<size_t>(z) * nx + x;
                        for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s)
                            if (!(cs_[s].flags & kWalkWall) && std::fabs(cs_[s].hi - k.pos[1]) + (x != cx || z != cz ? 0.05f : 0) < bd) { bd = std::fabs(cs_[s].hi - k.pos[1]); first = s; firstCell = static_cast<int32_t>(cell); }
                    }
            if (first == UINT32_MAX) continue;
            const uint8_t mark = k.erase ? 2 : 1;
            stack.assign(1, {first, firstCell});
            visit[first] = dab;
            painted[first] = mark;
            while (!stack.empty()) {
                const uint32_t s = stack.back().first;
                const int x = stack.back().second % nx, z = stack.back().second / nx;
                stack.pop_back();
                static const int d4[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& d : d4) {
                    const int xx = x + d[0], zz = z + d[1];
                    if (!inDab(xx, zz)) continue;
                    const size_t c2 = static_cast<size_t>(zz) * nx + xx;
                    uint32_t best = UINT32_MAX;
                    float bh = P.step + 1e-4f;
                    for (uint32_t s2 = cstart[c2]; s2 < cstart[c2 + 1]; ++s2)
                        if (!(cs_[s2].flags & kWalkWall) && std::fabs(cs_[s2].hi - cs_[s].hi) < bh) { bh = std::fabs(cs_[s2].hi - cs_[s].hi); best = s2; }
                    if (best == UINT32_MAX || visit[best] == dab) continue;
                    visit[best] = dab;
                    painted[best] = mark;
                    stack.push_back({best, static_cast<int32_t>(c2)});
                }
            }
        }
        for (size_t s = 0; s < cs_.size(); ++s) if (painted[s] == 1) cs_[s].flags |= kWalkUp | kWalkFloorRole;
    };
    compact();
    // A stroke that touched a see-through plant (the brush on a bush's top, a stroke saved before plants were gone
    // through) is on what's under it: the highest top below where it touched, up to a hedge's height down.
    std::vector<WalkStroke> grounded;
    if (paint) {
        auto leafTopAt = [&](size_t cell) {
            const int b = bandOfRow[cell / nx];
            const Raster& B = band[b];
            const size_t i = cell - static_cast<size_t>(firstRow(b)) * nx;
            return B.leafTop.empty() ? -1e30f : B.leafTop[i];
        };
        grounded = *paint;
        bool moved = false;
        for (WalkStroke& k : grounded) {
            const int cx = static_cast<int>(std::floor((k.pos[0] - R_ox) / cs)), cz = static_cast<int>(std::floor((k.pos[2] - R_oz) / cs));
            if (cx < 0 || cz < 0 || cx >= nx || cz >= nz) continue;
            const size_t cell = static_cast<size_t>(cz) * nx + cx;
            if (leafTopAt(cell) < k.pos[1] - 0.3f) continue;               // (no plant where it touched)
            bool onSomething = false;
            int under = -1;
            for (uint32_t sp = cstart[cell]; sp < cstart[cell + 1]; ++sp) {
                if (std::fabs(cs_[sp].hi - k.pos[1]) < 0.1f) onSomething = true;
                if (!(cs_[sp].flags & kWalkWall) && cs_[sp].hi <= k.pos[1] + 0.05f && cs_[sp].hi >= k.pos[1] - kPlantDrop) under = static_cast<int>(sp);
            }
            if (onSomething || under < 0) continue;
            k.pos[1] = cs_[under].hi;
            moved = true;
        }
        if (moved) paint = &grounded;
    }
    applyPaint();
    // Floor painted where there's nothing to paint on (over a gap, water, open air): a floor at the dab's height in each
    // of its cells with no surface within a step of it, so a floor can be carried on past its edge at its own level.
    // A dab that touched a surface makes it only over a drop (nothing within kOverDrop below): the brush's edge reaching
    // past a table top or a stair onto the floor under it paints no floor in the air round it.
    constexpr float kOverDrop = 1.0f;
    if (paint) {
        bool added = false;
        const auto anyNear = [&](size_t cell, float lo, float hi) {
            for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s) if (cs_[s].hi >= lo && cs_[s].lo <= hi) return true;
            return false;
        };
        for (const WalkStroke& k : *paint) {
            if (k.erase) continue;
            bool onSurface = false;   // (the dab's own cell: something within a step of where it touched)
            {
                const int cx = static_cast<int>(std::floor((k.pos[0] - R_ox) / cs)), cz = static_cast<int>(std::floor((k.pos[2] - R_oz) / cs));
                if (cx >= 0 && cz >= 0 && cx < nx && cz < nz) onSurface = anyNear(static_cast<size_t>(cz) * nx + cx, k.pos[1] - P.step, k.pos[1] + P.step);
            }
            forStrokeCells(k, R_ox, R_oz, cs, nx, nz, [&](int x, int z) {
                const size_t cell = static_cast<size_t>(z) * nx + x;
                if (anyNear(cell, k.pos[1] - P.step, k.pos[1] + P.step)) return;   // (something there: painted onto)
                if (onSurface && anyNear(cell, k.pos[1] - kOverDrop, k.pos[1])) return;   // (a floor just under it: no drop)
                addAt(cell, k.pos[1], k.pos[1], kWalkUp, -1);
                added = true;
            });
        }
        if (added) { compact(); applyPaint(); }
    }
    // 2. A span is a floor where it's stood on, isn't a wall, and has headroom.
    std::vector<uint8_t> cand;
    auto ceilOf = [&](size_t s, size_t cell) { return s + 1 < cstart[cell + 1] ? cs_[s + 1].lo : 1e30f; };
    auto findCand = [&] {
        cand.assign(cs_.size(), 0);
        for (size_t i = 0; i < ncell; ++i)
            for (uint32_t s = cstart[i]; s < cstart[i + 1]; ++s)
                cand[s] = (cs_[s].flags & kWalkUp) && !(cs_[s].flags & (kWalkWall | kWalkFurniture)) && painted[s] != 2 &&
                          ceilOf(s, i) - cs_[s].hi >= P.headroom;
    };
    findCand();

    // 3. Bridge seams and small holes: a cell between two floors at about the same height (along x, z or a diagonal,
    //    up to `gap` of missing cells) gets a floor at their height, when nothing stands in the body's way there.
    {
        const int gx = static_cast<int>(std::floor(gap / cs + 1e-3f)), gd = static_cast<int>(std::floor(gap / (cs * 1.41421f) + 1e-3f));
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
            applyPaint();
            findCand();
            for (size_t s = 0; s < cs_.size(); ++s) if (!cand[s]) cs_[s].flags &= static_cast<uint8_t>(~kWalkBridged);
        }
    }
    // The home's own floor: a cell is floor where its middle is on it (a cell the floor only reaches into is past its
    // edge). Floor painted or drawn by hand stays as it is.
    if (homeFloor)
        for (size_t i = 0; i < ncell; ++i) {
            const float x = R_ox + (static_cast<float>(i % nx) + 0.5f) * cs, z = R_oz + (static_cast<float>(i / nx) + 0.5f) * cs;
            for (uint32_t s = cstart[i]; s < cstart[i + 1]; ++s) {
                float y;
                if (cand[s] && !painted[s] && !(cs_[s].flags & kWalkFloorRole) && !probe.floorAt(x, z, cs_[s].hi, y)) cand[s] = 0;
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
    //     A long narrow top (under 75 cm across: a sofa's seat, the bench round a sunken lounge) is furniture however
    //     big, when every floor next door is 25 cm or more below it. And a top of something named a seat or a table
    //     (a sofa, a bench, a desk: WalkSource::hint) that stands a seat's height or more (30 cm to 1.3 m) straight
    //     over a floor beside it (not another seat) is furniture, whatever else it touches: the benches round a sunken
    //     lounge reach its steps, a tread's height off them, but you sit on them. Each mesh's level piece is weighed on
    //     its own (the steps beside the benches are another mesh, and stay), a stair's tread never stands that high
    //     over the floor next to it, and a big wide top (a loft, a stage) or a rim flush with a big floor stays floor.
    //     On the home's own floor only that named rule: a seat's or a table's top is never walked on, but the shape's
    //     guesses (the high steps cut, the plateaus peeled) are left out: what its makers made a floor a step or less
    //     up is one.
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
                if (homeFloor || b < 0 || plat[s] < 0 || plat[b] < 0 || plat[s] == plat[b]) continue;
                const float dh = std::fabs(cs_[b].hi - cs_[s].hi);
                const double small = std::min(parea[plat[s]], parea[plat[b]]);
                const double across = std::min(2 * parea[plat[s]] / std::max(cs, static_cast<float>(pedge[plat[s]])), 2 * parea[plat[b]] / std::max(cs, static_cast<float>(pedge[plat[b]])));
                if (dh > 0.23f && (small < 4.0 || across < 0.9) && !pfloorRole[plat[s]] && !pfloorRole[plat[b]])
                    con[s][d] = -1;                                    // (both ways: the other side meets this too)
            }
        std::vector<uint8_t> peeled(parea.size(), 0);
        std::vector<float> across(parea.size(), 0);                   // twice its area over its outline (m)
        for (size_t p = 0; p < parea.size(); ++p) across[p] = static_cast<float>(2 * parea[p] / std::max(static_cast<double>(cs), pedge[p]));
        {
            // what each source's name says it is, by its tag (the spans keep their top's tag)
            std::unordered_map<int, WalkHint> hintOf;
            for (const WalkSource& src : sources) if (src.tag >= 0 && src.hint != kHintNone) hintOf[src.tag] = src.hint;
            auto named = [&](size_t s) {
                const auto h = hintOf.find(cs_[s].tag);
                return h != hintOf.end() && (h->second == kHintSeat || h->second == kHintFurniture);
            };
            // straight down to a floor beside it: the next cell's highest top up to a cushion's bump over this one is
            // a floor (not another seat: the bench a stair runs down beside) a seat's height or more below; a backrest
            // or a railing there is higher: no drop
            auto drops = [&](size_t s, size_t cell) {
                const int x = static_cast<int>(cell % nx), z = static_cast<int>(cell / nx);
                const float y = cs_[s].hi;
                for (int d = 0; d < 4; ++d) {
                    const int xx = x + ddx[d], zz = z + ddz[d];
                    if (xx < 0 || zz < 0 || xx >= nx || zz >= nz) continue;
                    const size_t c2 = static_cast<size_t>(zz) * nx + xx;
                    int top = -1;
                    for (uint32_t s2 = cstart[c2]; s2 < cstart[c2 + 1]; ++s2) if (cs_[s2].hi <= y + 0.25f) top = static_cast<int>(s2);
                    if (top >= 0 && cand[top] && !named(static_cast<size_t>(top)) && cs_[top].hi <= y - 0.3f && cs_[top].hi >= y - 1.3f &&
                        (top + 1 >= static_cast<int>(cstart[c2 + 1]) || cs_[top + 1].lo > y + 0.25f)) return true;
                }
                return false;
            };
            // each level piece of one named mesh's top (a stair beside a bench, another mesh, is a piece of its own)
            std::vector<size_t> cellOf(nsp);
            for (size_t cell = 0; cell < ncell; ++cell) for (uint32_t s = cstart[cell]; s < cstart[cell + 1]; ++s) cellOf[s] = cell;
            std::vector<int32_t> piece(nsp, -1), stack, members;
            std::vector<uint8_t> gone(nsp, 0);
            for (size_t s0 = 0; s0 < nsp; ++s0) {
                if (!cand[s0] || piece[s0] >= 0 || pfloorRole[plat[s0]] || !named(s0)) continue;
                piece[s0] = static_cast<int32_t>(s0);
                stack.assign(1, static_cast<int32_t>(s0));
                members.clear();
                double area = 0, edge = 0;
                int dropping = 0;
                while (!stack.empty()) {
                    const int32_t s = stack.back();
                    stack.pop_back();
                    members.push_back(s);
                    area += cs * cs;
                    dropping += drops(static_cast<size_t>(s), cellOf[s]);
                    for (int d = 0; d < 4; ++d) {
                        const int32_t nb = con[s][d];
                        if (nb >= 0 && cs_[nb].tag == cs_[s].tag && std::fabs(cs_[nb].hi - cs_[s].hi) <= 0.05f) {
                            if (piece[nb] < 0) { piece[nb] = static_cast<int32_t>(s0); stack.push_back(nb); }
                        } else edge += cs;
                    }
                }
                const double acrossIt = 2 * area / std::max(static_cast<double>(cs), edge);
                // (a rim of it flush with a big floor, the edge of a sunken lounge at the room's own level: floor)
                const bool partOfFloor = parea[plat[s0]] >= 4.0 && parea[plat[s0]] > area * 2;
                if (dropping >= 2 && (area < 4.0 || acrossIt < 0.9) && !partOfFloor)
                    for (int32_t m : members) gone[m] = 1;
            }
            // gone: furniture, out of the plateaus' reckoning below (its links cut both ways)
            for (size_t s = 0; s < nsp; ++s) {
                if (!gone[s]) continue;
                cand[s] = 0;
                cs_[s].flags |= kWalkFurniture;
                for (int d = 0; d < 4; ++d) {
                    const int32_t nb = con[s][d];
                    if (nb >= 0) for (int e = 0; e < 4; ++e) if (con[nb][e] == static_cast<int32_t>(s)) con[nb][e] = -1;
                    con[s][d] = -1;
                }
            }
            // a plateau left with nothing (all of it a named seat's top): peeled with it
            std::vector<uint8_t> live(parea.size(), 0);
            for (size_t s = 0; s < nsp; ++s) if (plat[s] >= 0 && cand[s]) live[plat[s]] = 1;
            for (size_t p = 0; p < parea.size(); ++p) if (!live[p]) peeled[p] = 1;
        }
        for (int round = 0; round < 12 && !homeFloor; ++round) {
            // per plateau: any live floor next door, and is every one of them 12 cm or more below?
            std::vector<uint8_t> hasNb(parea.size(), 0), allLower(parea.size(), 1), allHigher(parea.size(), 1), allSeatLower(parea.size(), 1);
            for (size_t s = 0; s < nsp; ++s) {
                if (plat[s] < 0 || peeled[plat[s]] || !cand[s]) continue;
                for (int d = 0; d < 4; ++d) {
                    const int32_t b = con[s][d];
                    if (b < 0 || plat[b] < 0 || plat[b] == plat[s] || peeled[plat[b]] || parea[plat[b]] < 0.08) continue;   // (a bump isn't a floor)
                    hasNb[plat[s]] = 1;
                    if (cs_[b].hi > cs_[s].hi - 0.12f) allLower[plat[s]] = 0;
                    if (cs_[b].hi > cs_[s].hi - 0.25f) allSeatLower[plat[s]] = 0;
                    if (cs_[b].hi < cs_[s].hi + 0.12f) allHigher[plat[s]] = 0;
                }
            }
            bool any = false;
            for (size_t p = 0; p < parea.size(); ++p)
                if (!peeled[p] && hasNb[p] && !pfloorRole[p] &&
                    (((allLower[p] || allHigher[p]) && parea[p] < 4.0) || (allSeatLower[p] && across[p] < 0.75f))) { peeled[p] = 1; any = true; }
            if (!any) break;
        }
        for (size_t s = 0; s < nsp; ++s)
            if (plat[s] >= 0 && peeled[plat[s]]) { cand[s] = 0; cs_[s].flags |= kWalkFurniture; }
        for (size_t s = 0; s < nsp; ++s)                               // links to what's gone
            for (int d = 0; d < 4; ++d) if (con[s][d] >= 0 && !cand[con[s][d]]) con[s][d] = -1;
    }
    // 6 (after 5, below: run from there). Separate areas of the floors to stand on (core); keep the ones the player
    //    can get to.
    std::vector<uint8_t> core(nsp, 0);
    std::vector<int32_t> region(nsp, -1);
    std::vector<double> area;
    std::vector<uint8_t> keep;
    auto findAreas = [&] {
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
                if (cs_[s].flags & kWalkFloorRole) keep[id] |= painted[s] == 1 ? 4 : 1;   // (painted by hand: kept however small)
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
    };
    // The home's own floor: every floor is one to stand on (no body radius), so its areas are known before the
    // distances, and only the kept ones' edges are found exactly
    if (homeFloor) {
        core = cand;
        findAreas();
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
        struct Site { float x, z; uint32_t s; int8_t nx, nz; float lx = 0, lz = 0; bool exact = false; };   // n: the way out of the floor there; l: its line's normal
        std::vector<Site> sites;
        // On the home's own floor each one is where the floor really ends on the line out of its middle that way
        // (SurfaceProbe), as far as the next cell's middle (a mesh's wall passing through that cell took it all; past
        // its middle field() would carry the floor on into the wall). One whose end isn't on that line (a wall nearly
        // along it) is left out when its floor has an exact one, else stays on the cells' staircase, as do those beside
        // floor painted by hand.
        auto addSite = [&](size_t s, int ex, int ez) {
            Site t{spanX[s] + 0.5f + 0.5f * ex, spanZ[s] + 0.5f + 0.5f * ez, static_cast<uint32_t>(s), static_cast<int8_t>(ex), static_cast<int8_t>(ez)};
            if (homeFloor && keep[region[s]]) {
                const int ox2 = spanX[s] + ex, oz2 = spanZ[s] + ez;
                bool byPaint = painted[s] || (cs_[s].flags & kWalkFloorRole);
                if (ox2 >= 0 && oz2 >= 0 && ox2 < nx && oz2 < nz) {
                    const size_t c2 = static_cast<size_t>(oz2) * nx + ox2;
                    for (uint32_t q = cstart[c2]; q < cstart[c2 + 1]; ++q) byPaint |= painted[q] != 0;
                }
                const float wx = R_ox + (spanX[s] + 0.5f) * cs, wz = R_oz + (spanZ[s] + 0.5f) * cs;
                float y;
                if (!byPaint && probe.floorAt(wx, wz, cs_[s].hi, y)) {
                    const float L = std::sqrt(static_cast<float>(ex * ex + ez * ez)), ux = ex / L, uz = ez / L, len = cs * L;
                    const float at = probe.edgeAlong(wx, wz, ux, uz, len, y);
                    if (at <= len) { t.x = spanX[s] + 0.5f + ux * at / cs; t.z = spanZ[s] + 0.5f + uz * at / cs; t.exact = true; }
                }
            }
            sites.push_back(t);
        };
        for (size_t s = 0; s < nsp; ++s) {
            if (!cand[s]) continue;
            const size_t first = sites.size();
            for (int d = 0; d < 4; ++d) {
                if (con[s][d] < 0) { addSite(s, ddx[d], ddz[d]); continue; }
                const int d2 = (d + 1) & 3;                            // the corner between d and the next direction
                if (con[s][d2] < 0) continue;
                if (con[con[s][d]][d2] < 0 && con[con[s][d2]][d] < 0) addSite(s, ddx[d] + ddx[d2], ddz[d] + ddz[d2]);
            }
            if (homeFloor && std::any_of(sites.begin() + first, sites.end(), [](const Site& t) { return t.exact; }))
                sites.erase(std::remove_if(sites.begin() + first, sites.end(), [](const Site& t) { return !t.exact; }), sites.end());
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
        std::vector<uint32_t> nearby;   // (not "near": windows.h defines that away)
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
            nearby.clear();
            for (int bz = std::max(0, static_cast<int>(std::floor(a.z - kR))); bz <= std::min(nz - 1, static_cast<int>(std::floor(a.z + kR))); ++bz)
                for (int bx = std::max(0, static_cast<int>(std::floor(a.x - kR))); bx <= std::min(nx - 1, static_cast<int>(std::floor(a.x + kR))); ++bx) {
                    const size_t c = static_cast<size_t>(bz) * nx + bx;
                    for (uint32_t k = cellStart[c]; k < cellStart[c + 1]; ++k) {
                        const uint32_t j = byCell[k];
                        const Site& b = sites[j];
                        if ((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z) > kR * kR || std::fabs(cs_[b.s].hi - ha) > P.step) continue;
                        if (mark[b.s] != epoch || b.nx * a.nx + b.nz * a.nz < 0) continue;   // another edge (past a wall)
                        nearby.push_back(j); n += 1; mx += b.x; mz += b.z;
                    }
                }
            if (n < 6) continue;
            mx /= n; mz /= n;
            double sxx = 0, szz = 0, sxz = 0;
            for (uint32_t j : nearby) { const double dx = sites[j].x - mx, dz = sites[j].z - mz; sxx += dx * dx; szz += dz * dz; sxz += dx * dz; }
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
    const int wallReach = static_cast<int>(std::ceil(radius / cs));
    auto wallNear = [&](size_t s) {
        const float h = cs_[s].hi, reach = radius + cs;
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
    for (size_t s = 0; s < nsp; ++s) {
        if (!cand[s]) { dist[s] = 0; continue; }
        dist[s] = std::sqrt(std::min(dist[s], 1e8f)) * cs;
        // A drawn floor's edge is where it ends (walkable right up to it), but a wall standing on it still keeps
        // the body its radius away: one drawn through it takes that floor away.
        if ((cs_[s].flags & kWalkFloorRole) && !wallNear(s)) dist[s] = std::max(dist[s], 2 * radius);
        core[s] = dist[s] >= radius - 1e-4f;   // (the home's own floor: every one, as before the distances)
    }
    if (!homeFloor) findAreas();
    // The floors round a kept area that are too near its edge to stand on: the edge runs over them. Only those a body
    // standing in the area can cover: inside the clear disc round some standing spot (a core floor's distance to the
    // edge), so the area runs right up to a wall but a sliver of floor seen through a gap narrower than the body (under
    // furniture, between slats) is left out, not drawn as a knob poking into it.
    const int coverReach = static_cast<int>(std::ceil((radius + 1.5f * cs) / cs));
    const float coverSlack = 0.25f * cs;   // the cells' centres don't sit on the edge: a quarter cell either way
    auto covered = [&](size_t s) {
        const float h = cs_[s].hi;
        for (int dz = -coverReach; dz <= coverReach; ++dz)
            for (int dx = -coverReach; dx <= coverReach; ++dx) {
                const int x = spanX[s] + dx, z = spanZ[s] + dz;
                if (x < 0 || z < 0 || x >= nx || z >= nz) continue;
                const float apart = std::sqrt(static_cast<float>(dx * dx + dz * dz)) * cs;
                const size_t cell = static_cast<size_t>(z) * nx + x;
                for (uint32_t c = cstart[cell]; c < cstart[cell + 1]; ++c)
                    if (core[c] && keep[region[c]] && std::fabs(cs_[c].hi - h) <= P.step && apart <= dist[c] + coverSlack) return true;
            }
        return false;
    };
    std::vector<uint8_t> nearEdge(nsp, 0);
    {
        std::vector<int32_t> ring, nextRing;
        for (size_t s = 0; s < nsp; ++s) if (core[s] && keep[region[s]]) ring.push_back(static_cast<int32_t>(s));
        const int depth = static_cast<int>(std::ceil(radius / cs)) + 1;
        for (int k = 0; k < depth && !ring.empty(); ++k) {
            nextRing.clear();
            for (int32_t s : ring)
                for (int d = 0; d < 4; ++d) {
                    const int32_t b = con[s][d];
                    if (b >= 0 && cand[b] && !core[b] && !nearEdge[b] && covered(static_cast<size_t>(b))) { nearEdge[b] = 1; nextRing.push_back(b); }
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
        const int reachCells = static_cast<int>(std::ceil(radius / cs)) + 3;
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
        {   // what its name says it is: a Studio surface's mesh ("<index>:<name>"), else its own name
            const size_t colon = c.mesh.find(':');
            if (c.mesh.find("#floor") == std::string::npos) s.hint = walkNameHint(colon != std::string::npos ? c.mesh.substr(colon + 1) : c.name);
        }
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
