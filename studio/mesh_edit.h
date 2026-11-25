// Edit mode for a mesh collider (a navmesh, one of Meta's collision meshes, a drawn floor): Frame Studio's Tab on a
// selected one. Its corners are picked in the 3D view (click, Shift+click to add or take away, drag a box, A for all)
// and moved, turned or scaled with the gizmo; X deletes them (and the faces on them), F makes a face of them, E
// extrudes their open edges, M merges them into one, L selects everything joined to them. Corners at the same place
// (a mesh split at its seams) are one corner here and move together.
// Needs imgui.h and ImGuizmo.h, and world.h's WorldCollider, included before it.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <climits>
#include <functional>
#include <unordered_map>
#include <vector>

class MeshEditor {
public:
    // Where the 3D view is: its matrices (column-major) and its rectangle on screen.
    struct View {
        const float* view = nullptr;
        const float* proj = nullptr;
        const float* viewProj = nullptr;
        ImVec2 pos, size;
        float nearW = 0.05f;
        float scale = 1;
    };

    bool active() const { return index_ >= 0; }
    int collider() const { return index_; }
    size_t selectedCount() const { size_t n = 0; for (char s : sel_) n += s != 0; return n; }
    size_t cornerCount() const { return groupFirst_.size(); }

    void begin(int index, const WorldCollider& c) {
        index_ = index;
        rebuild(c);
        std::fill(sel_.begin(), sel_.end(), 0);
        boxing_ = false;
        dragging_ = false;
    }
    void end() { index_ = -1; sel_.clear(); group_.clear(); groupFirst_.clear(); boxing_ = dragging_ = false; }
    // The collider's index moved (one before it deleted) or it went: kept up to date by the caller.
    void renumber(int index) { index_ = index; }

    // One frame in the 3D view: the mesh drawn (edges, corners, the selection), the mouse taken (picking, the box), the
    // gizmo on the selection (op: ImGuizmo's TRANSLATE, ROTATE or SCALE). beforeChange runs once before each change
    // (the caller's undo step). Returns true when the mesh changed; overUi: the gizmo has the mouse.
    bool frame(WorldCollider& c, ImDrawList* dl, const View& v, bool hovered, bool gizmoAllowed, int op, bool snap,
               const std::function<void()>& beforeChange, bool& overUi) {
        overUi = false;
        if (!active()) return false;
        if (group_.size() != c.positions.size() / 3) rebuild(c);
        world(c);
        // the edges, then the corners (the selected ones in orange)
        const ImU32 edgeCol = IM_COL32(120, 220, 255, 200), dotCol = IM_COL32(20, 20, 24, 255), selCol = IM_COL32(255, 160, 51, 255);
        for (size_t t = 0; t + 2 < c.indices.size(); t += 3)
            for (int k = 0; k < 3; ++k) {
                const uint32_t a = c.indices[t + k], b = c.indices[t + (k + 1) % 3];
                if (a >= wpos_.size() / 3 || b >= wpos_.size() / 3 || group_[a] > group_[b]) continue;   // (each shared edge once, mostly)
                line(dl, v, &wpos_[a * 3], &wpos_[b * 3], edgeCol, 1.2f * v.scale);
            }
        std::vector<ImVec2> at(groupFirst_.size());
        std::vector<char> seen(groupFirst_.size(), 0);
        for (size_t g = 0; g < groupFirst_.size(); ++g) {
            seen[g] = project(v, &wpos_[groupFirst_[g] * 3], at[g]);
            if (!seen[g]) continue;
            const float r = (sel_[g] ? 4.0f : 3.0f) * v.scale;
            dl->AddRectFilled(ImVec2(at[g].x - r, at[g].y - r), ImVec2(at[g].x + r, at[g].y + r), sel_[g] ? selCol : dotCol);
            if (!sel_[g]) dl->AddRect(ImVec2(at[g].x - r, at[g].y - r), ImVec2(at[g].x + r, at[g].y + r), edgeCol);
        }
        bool changed = false;
        // the gizmo on the selected corners' middle
        const size_t nsel = selectedCount();
        if (nsel) {
            float mid[3] = {0, 0, 0};
            for (size_t g = 0; g < groupFirst_.size(); ++g)
                if (sel_[g]) for (int k = 0; k < 3; ++k) mid[k] += wpos_[groupFirst_[g] * 3 + k] / static_cast<float>(nsel);
            if (!dragging_) {
                for (int k = 0; k < 16; ++k) gizmo_[k] = (k % 5 == 0) ? 1.0f : 0.0f;
                std::copy_n(mid, 3, gizmo_ + 12);
            }
            const float before[16] = {gizmo_[0], gizmo_[1], gizmo_[2], gizmo_[3], gizmo_[4], gizmo_[5], gizmo_[6], gizmo_[7],
                                      gizmo_[8], gizmo_[9], gizmo_[10], gizmo_[11], gizmo_[12], gizmo_[13], gizmo_[14], gizmo_[15]};
            ImGuizmo::SetOrthographic(false);
            ImGuizmo::SetDrawlist(dl);
            ImGuizmo::SetRect(v.pos.x, v.pos.y, v.size.x, v.size.y);
            ImGuizmo::Enable(gizmoAllowed || ImGuizmo::IsUsing());
            const float snaps[3] = {op == ImGuizmo::ROTATE ? 15.0f : op == ImGuizmo::SCALE ? 0.1f : 0.05f, op == ImGuizmo::ROTATE ? 15.0f : op == ImGuizmo::SCALE ? 0.1f : 0.05f,
                                    op == ImGuizmo::ROTATE ? 15.0f : op == ImGuizmo::SCALE ? 0.1f : 0.05f};
            const bool moved = ImGuizmo::Manipulate(v.view, v.proj, static_cast<ImGuizmo::OPERATION>(op), ImGuizmo::WORLD, gizmo_, nullptr, snap ? snaps : nullptr);
            const bool using_ = ImGuizmo::IsUsing();
            if (using_ && !dragging_) { beforeChange(); dragging_ = true; }
            if (!using_) dragging_ = false;
            if (moved && using_) {
                // this frame's change: after * inverse(before), applied to the selected corners in the world
                float inv[16], delta[16];
                if (invertAffine(before, inv)) {
                    mul(gizmo_, inv, delta);
                    float toLocal[16];
                    if (invertAffine(c.world, toLocal))
                        for (size_t vtx = 0; vtx < group_.size(); ++vtx) {
                            if (!sel_[group_[vtx]]) continue;
                            float p[3], lp[3];
                            xf(delta, &wpos_[vtx * 3], p);
                            xf(toLocal, p, lp);
                            if (std::isfinite(lp[0]) && std::isfinite(lp[1]) && std::isfinite(lp[2])) std::copy_n(lp, 3, &c.positions[vtx * 3]);
                        }
                    world(c);
                    changed = true;
                }
            }
            overUi = ImGuizmo::IsOver() || using_;
        }
        // picking: a click on a corner selects it (Shift or Ctrl: adds or takes it away); a drag from anywhere else is a box
        ImGuiIO& io = ImGui::GetIO();
        if (hovered && !overUi && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
            boxing_ = true;
            boxFrom_ = io.MousePos;
        }
        if (boxing_) {
            const ImVec2 a = boxFrom_, b = io.MousePos;
            const bool dragged = std::fabs(a.x - b.x) > 4 * v.scale || std::fabs(a.y - b.y) > 4 * v.scale;
            if (dragged) {
                dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(255, 160, 51, 30));
                dl->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), selCol);
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                boxing_ = false;
                const bool add = io.KeyShift, take = io.KeyCtrl;
                if (dragged) {
                    if (!add && !take) std::fill(sel_.begin(), sel_.end(), 0);
                    const float x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
                    for (size_t g = 0; g < at.size(); ++g)
                        if (seen[g] && at[g].x >= x0 && at[g].x <= x1 && at[g].y >= y0 && at[g].y <= y1) sel_[g] = take ? 0 : 1;
                } else {
                    int best = -1;
                    float bd = 10 * v.scale;
                    for (size_t g = 0; g < at.size(); ++g) {
                        if (!seen[g]) continue;
                        const float d = std::hypot(at[g].x - b.x, at[g].y - b.y);
                        if (d < bd) { bd = d; best = static_cast<int>(g); }
                    }
                    if (best >= 0) {
                        if (add || take) sel_[best] = sel_[best] ? 0 : 1;
                        else { std::fill(sel_.begin(), sel_.end(), 0); sel_[best] = 1; }
                    } else if (!add && !take) std::fill(sel_.begin(), sel_.end(), 0);
                }
            }
        }
        return changed;
    }

    void selectAll(bool on) { std::fill(sel_.begin(), sel_.end(), on ? 1 : 0); }
    void toggleAll() { selectAll(selectedCount() == 0); }

    // Everything joined (by faces) to what's selected.
    void selectLinked(const WorldCollider& c) {
        bool grew = true;
        while (grew) {
            grew = false;
            for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
                bool any = false, all = true;
                for (int k = 0; k < 3; ++k) { const char s = sel_[group_[c.indices[t + k]]]; any |= s != 0; all &= s != 0; }
                if (any && !all) { for (int k = 0; k < 3; ++k) sel_[group_[c.indices[t + k]]] = 1; grew = true; }
            }
        }
    }

    // X: the selected corners go, and every face on one of them.
    bool deleteSelected(WorldCollider& c) {
        if (!selectedCount()) return false;
        std::vector<uint32_t> keep;
        for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
            bool hit = false;
            for (int k = 0; k < 3; ++k) hit |= sel_[group_[c.indices[t + k]]] != 0;
            if (!hit) keep.insert(keep.end(), {c.indices[t], c.indices[t + 1], c.indices[t + 2]});
        }
        c.indices.swap(keep);
        compact(c);
        rebuild(c);
        std::fill(sel_.begin(), sel_.end(), 0);
        return true;
    }

    // F: a face of the selected corners (three: a triangle; more: a fan round their middle, in order round it), facing
    // up where it can (a floor), else the way the faces round it do.
    bool fill(WorldCollider& c) {
        std::vector<uint32_t> g;
        for (size_t i = 0; i < sel_.size(); ++i) if (sel_[i]) g.push_back(static_cast<uint32_t>(i));
        if (g.size() < 3) return false;
        world(c);
        float mid[3] = {0, 0, 0};
        for (uint32_t i : g) for (int k = 0; k < 3; ++k) mid[k] += wpos_[groupFirst_[i] * 3 + k] / static_cast<float>(g.size());
        // their plane's normal (Newell's, over them in the order found, then in angle order)
        float n[3] = {0, 1, 0};
        {
            float acc[3] = {0, 0, 0};
            for (size_t i = 0; i < g.size(); ++i) {
                const float* a = &wpos_[groupFirst_[g[i]] * 3];
                const float* b = &wpos_[groupFirst_[g[(i + 1) % g.size()]] * 3];
                acc[0] += (a[1] - b[1]) * (a[2] + b[2]); acc[1] += (a[2] - b[2]) * (a[0] + b[0]); acc[2] += (a[0] - b[0]) * (a[1] + b[1]);
            }
            const float l = std::sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
            if (l > 1e-9f && std::fabs(acc[1]) / l < 0.7f) for (int k = 0; k < 3; ++k) n[k] = acc[k] / l;
        }
        if (n[1] < 0) for (float& x : n) x = -x;
        float u[3] = {1, 0, 0}, w[3];
        if (std::fabs(n[0]) > 0.9f) { u[0] = 0; u[1] = 1; }
        cross(n, u, w); normalize(w); cross(w, n, u); normalize(u);
        std::sort(g.begin(), g.end(), [&](uint32_t a, uint32_t b) {
            const float* pa = &wpos_[groupFirst_[a] * 3];
            const float* pb = &wpos_[groupFirst_[b] * 3];
            const float da[3] = {pa[0] - mid[0], pa[1] - mid[1], pa[2] - mid[2]}, db[3] = {pb[0] - mid[0], pb[1] - mid[1], pb[2] - mid[2]};
            return std::atan2(dot(da, w), dot(da, u)) < std::atan2(db[0] * w[0] + db[1] * w[1] + db[2] * w[2], dot(db, u));
        });
        // (atan2(w, u) increasing is clockwise seen from n's side for u x w = -n: wound so the face looks along n)
        auto add = [&](uint32_t a, uint32_t b, uint32_t d) {
            float e1[3], e2[3], f[3];
            for (int k = 0; k < 3; ++k) { e1[k] = wpos_[b * 3 + k] - wpos_[a * 3 + k]; e2[k] = wpos_[d * 3 + k] - wpos_[a * 3 + k]; }
            cross(e1, e2, f);
            if (dot(f, n) < 0) std::swap(b, d);
            c.indices.insert(c.indices.end(), {a, b, d});
        };
        if (g.size() == 3) add(groupFirst_[g[0]], groupFirst_[g[1]], groupFirst_[g[2]]);
        else
            for (size_t i = 1; i + 1 < g.size(); ++i) add(groupFirst_[g[0]], groupFirst_[g[i]], groupFirst_[g[i + 1]]);
        return true;
    }

    // E: the selected corners' open edges (on one face only) drawn out into new faces; the new corners are selected,
    // where the old ones were, for the gizmo to move. A corner with no open edge is copied alone.
    bool extrude(WorldCollider& c) {
        if (!selectedCount()) return false;
        std::unordered_map<uint64_t, int> uses;
        auto key = [](uint32_t a, uint32_t b) { return a < b ? (static_cast<uint64_t>(a) << 32) | b : (static_cast<uint64_t>(b) << 32) | a; };
        for (size_t t = 0; t + 2 < c.indices.size(); t += 3)
            for (int k = 0; k < 3; ++k) ++uses[key(group_[c.indices[t + k]], group_[c.indices[t + (k + 1) % 3]])];
        std::unordered_map<uint32_t, uint32_t> copy;   // group -> new vertex
        auto copyOf = [&](uint32_t g) {
            auto it = copy.find(g);
            if (it != copy.end()) return it->second;
            const uint32_t nv = static_cast<uint32_t>(c.positions.size() / 3);
            const float p[3] = {c.positions[groupFirst_[g] * 3], c.positions[groupFirst_[g] * 3 + 1], c.positions[groupFirst_[g] * 3 + 2]};
            c.positions.insert(c.positions.end(), p, p + 3);
            copy.emplace(g, nv);
            return nv;
        };
        const size_t faces = c.indices.size();
        for (size_t t = 0; t + 2 < faces; t += 3)
            for (int k = 0; k < 3; ++k) {
                const uint32_t va = c.indices[t + k], vb = c.indices[t + (k + 1) % 3];
                const uint32_t ga = group_[va], gb = group_[vb];
                if (!sel_[ga] || !sel_[gb] || uses[key(ga, gb)] != 1) continue;
                // the face runs a -> b along this edge: the new face runs b -> a on its side, then out
                const uint32_t na = copyOf(ga), nb = copyOf(gb);
                c.indices.insert(c.indices.end(), {vb, va, na, vb, na, nb});
            }
        const size_t firstNew = groupFirst_.empty() ? 0 : group_.size();
        for (size_t g = 0; g < sel_.size(); ++g) if (sel_[g]) copyOf(static_cast<uint32_t>(g));
        rebuild(c, firstNew);   // (the copies are corners of their own, where the old ones are, until they're moved)
        std::fill(sel_.begin(), sel_.end(), 0);
        for (const auto& [g, nv] : copy) sel_[group_[nv]] = 1;
        return true;
    }

    // M: the selected corners made one, at their middle; faces left with no area go.
    bool merge(WorldCollider& c) {
        std::vector<uint32_t> g;
        for (size_t i = 0; i < sel_.size(); ++i) if (sel_[i]) g.push_back(static_cast<uint32_t>(i));
        if (g.size() < 2) return false;
        float mid[3] = {0, 0, 0};
        for (uint32_t i : g) for (int k = 0; k < 3; ++k) mid[k] += c.positions[groupFirst_[i] * 3 + k] / static_cast<float>(g.size());
        const uint32_t to = groupFirst_[g[0]];
        std::copy_n(mid, 3, &c.positions[to * 3]);
        for (uint32_t& x : c.indices) if (sel_[group_[x]]) x = to;
        std::vector<uint32_t> keep;
        for (size_t t = 0; t + 2 < c.indices.size(); t += 3) {
            const uint32_t a = c.indices[t], b = c.indices[t + 1], d = c.indices[t + 2];
            if (a != b && b != d && a != d) keep.insert(keep.end(), {a, b, d});
        }
        c.indices.swap(keep);
        compact(c);
        rebuild(c);
        std::fill(sel_.begin(), sel_.end(), 0);
        if (!group_.empty()) {
            // (the merged corner selected again: nearest to the middle)
            size_t best = 0;
            float bd = 1e30f;
            for (size_t vtx = 0; vtx < group_.size(); ++vtx) {
                const float d = std::fabs(c.positions[vtx * 3] - mid[0]) + std::fabs(c.positions[vtx * 3 + 1] - mid[1]) + std::fabs(c.positions[vtx * 3 + 2] - mid[2]);
                if (d < bd) { bd = d; best = vtx; }
            }
            sel_[group_[best]] = 1;
        }
        return true;
    }

private:
    int index_ = -1;
    std::vector<uint32_t> group_;        // per vertex: its corner (vertices at one place share it)
    std::vector<uint32_t> groupFirst_;   // per corner: its first vertex
    std::vector<char> sel_;              // per corner
    std::vector<float> wpos_;            // the vertices in the world
    bool boxing_ = false, dragging_ = false;
    ImVec2 boxFrom_;
    float gizmo_[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

    static float dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
    static void cross(const float* a, const float* b, float* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
    static void normalize(float* a) { const float l = std::sqrt(dot(a, a)); if (l > 1e-12f) for (int k = 0; k < 3; ++k) a[k] /= l; }
    static void xf(const float* m, const float* p, float* o) { for (int k = 0; k < 3; ++k) o[k] = m[k] * p[0] + m[4 + k] * p[1] + m[8 + k] * p[2] + m[12 + k]; }
    static void mul(const float* a, const float* b, float* o) {   // o = a * b (column-major)
        for (int col = 0; col < 4; ++col)
            for (int row = 0; row < 4; ++row) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += a[k * 4 + row] * b[col * 4 + k];
                o[col * 4 + row] = s;
            }
    }
    static bool invertAffine(const float* m, float* o) {
        const float a = m[0], b = m[4], cc = m[8], d = m[1], e = m[5], f = m[9], g = m[2], h = m[6], i = m[10];
        const float det = a * (e * i - f * h) - b * (d * i - f * g) + cc * (d * h - e * g);
        if (!(std::fabs(det) > 1e-12f)) return false;
        const float r = 1 / det;
        const float inv[9] = {(e * i - f * h) * r, (cc * h - b * i) * r, (b * f - cc * e) * r, (f * g - d * i) * r, (a * i - cc * g) * r, (cc * d - a * f) * r,
                              (d * h - e * g) * r, (b * g - a * h) * r, (a * e - b * d) * r};   // row-major 3x3
        for (int row = 0; row < 3; ++row) for (int col = 0; col < 3; ++col) o[col * 4 + row] = inv[row * 3 + col];
        for (int row = 0; row < 3; ++row) o[12 + row] = -(o[row] * m[12] + o[4 + row] * m[13] + o[8 + row] * m[14]);
        o[3] = o[7] = o[11] = 0;
        o[15] = 1;
        return true;
    }
    static bool project(const View& v, const float* p, ImVec2& out) {
        float c[4];
        for (int k = 0; k < 4; ++k) c[k] = v.viewProj[k] * p[0] + v.viewProj[4 + k] * p[1] + v.viewProj[8 + k] * p[2] + v.viewProj[12 + k];
        if (c[3] < v.nearW) return false;
        out = ImVec2(v.pos.x + (c[0] / c[3] * 0.5f + 0.5f) * v.size.x, v.pos.y + (0.5f - c[1] / c[3] * 0.5f) * v.size.y);
        return true;
    }
    static void line(ImDrawList* dl, const View& v, const float* a, const float* b, ImU32 color, float t) {
        float ca[4], cb[4];
        for (int k = 0; k < 4; ++k) {
            ca[k] = v.viewProj[k] * a[0] + v.viewProj[4 + k] * a[1] + v.viewProj[8 + k] * a[2] + v.viewProj[12 + k];
            cb[k] = v.viewProj[k] * b[0] + v.viewProj[4 + k] * b[1] + v.viewProj[8 + k] * b[2] + v.viewProj[12 + k];
        }
        if (ca[3] < v.nearW && cb[3] < v.nearW) return;
        if (ca[3] < v.nearW || cb[3] < v.nearW) {   // (cut where it crosses the near plane)
            float* in = ca[3] < v.nearW ? cb : ca;
            float* out = ca[3] < v.nearW ? ca : cb;
            const float s = (in[3] - v.nearW) / (in[3] - out[3]);
            for (int k = 0; k < 4; ++k) out[k] = in[k] + (out[k] - in[k]) * s;
        }
        auto scr = [&](const float* q) { return ImVec2(v.pos.x + (q[0] / q[3] * 0.5f + 0.5f) * v.size.x, v.pos.y + (0.5f - q[1] / q[3] * 0.5f) * v.size.y); };
        dl->AddLine(scr(ca), scr(cb), color, t);
    }
    void world(const WorldCollider& c) {
        wpos_.resize(c.positions.size());
        for (size_t i = 0; i + 2 < c.positions.size(); i += 3) xf(c.world, &c.positions[i], &wpos_[i]);
    }
    // The corners: vertices within half a millimetre of each other are one (those from splitFrom on: only with each other).
    void rebuild(const WorldCollider& c, size_t splitFrom = SIZE_MAX) {
        const size_t nv = c.positions.size() / 3;
        group_.assign(nv, 0);
        groupFirst_.clear();
        std::unordered_map<uint64_t, uint32_t> at;
        for (size_t v = 0; v < nv; ++v) {
            uint64_t k = 0;
            for (int j = 0; j < 3; ++j) k = k * 2654435761ull + static_cast<uint64_t>(static_cast<int64_t>(std::llround(c.positions[v * 3 + j] * 2000.0)));
            if (v >= splitFrom) k ^= 0x9e3779b97f4a7c15ull;
            const auto it = at.emplace(k, static_cast<uint32_t>(groupFirst_.size())).first;
            if (it->second == groupFirst_.size()) groupFirst_.push_back(static_cast<uint32_t>(v));
            group_[v] = it->second;
        }
        sel_.assign(groupFirst_.size(), 0);
        world(c);
    }
    // Vertices no face uses go (and the indices follow).
    static void compact(WorldCollider& c) {
        std::vector<uint32_t> remap(c.positions.size() / 3, UINT32_MAX);
        std::vector<float> pos;
        for (uint32_t& x : c.indices) {
            if (remap[x] == UINT32_MAX) { remap[x] = static_cast<uint32_t>(pos.size() / 3); pos.insert(pos.end(), &c.positions[x * 3], &c.positions[x * 3] + 3); }
            x = remap[x];
        }
        c.positions.swap(pos);
    }
};
