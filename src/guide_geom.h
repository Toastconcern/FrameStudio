#pragma once
// Guide geometry: what Frame Home draws itself to show you where you're going (the teleport arc and its landing
// marker, the seats you can sit in, the pointer on a seat). Built on the CPU each frame as plain world-space
// triangles with a colour each, and drawn by HslRenderer::setGuide over the scene (depth tested, so walls hide it,
// with a faint copy where it's hidden). It's Frame Home's own frame, so it shows in system mode too, where
// SteamVR overlays of a non-scene app can't be relied on.
#include <algorithm>
#include <cmath>
#include <vector>

struct GuideVertex { float pos[3]; float rgba[4]; };   // world (home) space; linear colour, straight alpha

// What locomotion wants shown this frame (Locomotion::guide()).
struct LocoGuide {
    bool aiming = false, valid = false, seat = false;   // the arc is up; it lands; it lands on a seat
    std::vector<float> arc;                              // xyz points, the hand first, the end last
    float target[3] = {0, 0, 0}, normal[3] = {0, 1, 0};  // landing point and its surface's normal
    float aimTime = 0;                                   // seconds the arc has been up (it fades in)
    struct Seat { float pos[3]; float yaw; bool hot; float near = 1; };   // hot: pointed at (or the arc lands on it); near: 0..1 fade
    std::vector<Seat> seats;                             // seats near you (not the one you sit in)
    bool pointer = false;                                // a hand points at a seat: a short line to it
    float pointerFrom[3] = {0, 0, 0}, pointerTo[3] = {0, 0, 0};
};

namespace guide {

struct P { float x, y, z; };
inline P operator+(P a, P b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline P operator-(P a, P b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline P operator*(P a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(P a, P b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline P cross(P a, P b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline P norm(P a) { const float l = std::sqrt(dot(a, a)); return l > 1e-9f ? a * (1 / l) : P{0, 1, 0}; }
inline float smooth(float e0, float e1, float x) { const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); }

struct C { float r, g, b, a; };
inline C mix(C a, C b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t}; }
inline C alpha(C c, float a) { return {c.r, c.g, c.b, c.a * a}; }

inline void vert(std::vector<GuideVertex>& o, P p, C c) { o.push_back({{p.x, p.y, p.z}, {c.r, c.g, c.b, c.a}}); }
inline void tri(std::vector<GuideVertex>& o, P a, C ca, P b, C cb, P c, C cc) { vert(o, a, ca); vert(o, b, cb); vert(o, c, cc); }
inline void quad(std::vector<GuideVertex>& o, P a, C ca, P b, C cb, P c, C cc, P d, C cd) { tri(o, a, ca, b, cb, c, cc); tri(o, a, ca, c, cc, d, cd); }

// Two axes across a surface with normal n.
inline void basis(P n, P& u, P& w) {
    u = norm(cross(std::fabs(n.y) < 0.9f ? P{0, 1, 0} : P{1, 0, 0}, n));
    w = cross(n, u);
}

// A flat band between radii r0 and r1 around c (in the u/w plane), soft-edged: transparent at both rims, `col`
// along its middle.
inline void ring(std::vector<GuideVertex>& o, P c, P u, P w, float r0, float r1, C col, int segs = 64) {
    const float rm = (r0 + r1) * 0.5f;
    const C clear = alpha(col, 0);
    for (int i = 0; i < segs; ++i) {
        const float a0 = 6.2831853f * i / segs, a1 = 6.2831853f * (i + 1) / segs;
        const P d0 = u * std::cos(a0) + w * std::sin(a0), d1 = u * std::cos(a1) + w * std::sin(a1);
        quad(o, c + d0 * r0, clear, c + d1 * r0, clear, c + d1 * rm, col, c + d0 * rm, col);
        quad(o, c + d0 * rm, col, c + d1 * rm, col, c + d1 * r1, clear, c + d0 * r1, clear);
    }
}

// A filled disc, `mid` at the centre fading to `rim` at radius r.
inline void disc(std::vector<GuideVertex>& o, P c, P u, P w, float r, C mid, C rim, int segs = 48) {
    for (int i = 0; i < segs; ++i) {
        const float a0 = 6.2831853f * i / segs, a1 = 6.2831853f * (i + 1) / segs;
        tri(o, c, mid, c + (u * std::cos(a0) + w * std::sin(a0)) * r, rim, c + (u * std::cos(a1) + w * std::sin(a1)) * r, rim);
    }
}

// The comfort vignette: the edges of view darkened by `amount` (0..1) while you move without moving yourself. A band
// a hand's width in front of the eyes, facing along f (u, w across it): clear inside `inner`, dark from `outer` (both
// tangents of the angle off the view's centre) out past the edge of view.
inline void vignette(std::vector<GuideVertex>& o, P eye, P f, P u, P w, float inner, float outer, float amount, int segs = 48) {
    const float d = 0.25f;
    const P c = eye + f * d;
    const C clear{0, 0, 0, 0}, dark{0, 0, 0, amount};
    for (int i = 0; i < segs; ++i) {
        const float a0 = 6.2831853f * i / segs, a1 = 6.2831853f * (i + 1) / segs;
        const P d0 = u * std::cos(a0) + w * std::sin(a0), d1 = u * std::cos(a1) + w * std::sin(a1);
        quad(o, c + d0 * (inner * d), clear, c + d1 * (inner * d), clear, c + d1 * (outer * d), dark, c + d0 * (outer * d), dark);
        quad(o, c + d0 * (outer * d), dark, c + d1 * (outer * d), dark, c + d1 * (4.0f * d), dark, c + d0 * (4.0f * d), dark);
    }
}

// A chevron (an arrowhead ">") pointing along f in the plane of f and s, its tip at `tip`.
inline void chevron(std::vector<GuideVertex>& o, P tip, P f, P s, float size, float thick, C col) {
    const C clear = alpha(col, 0);
    for (float side : {-1.0f, 1.0f}) {
        const P a = tip, b = tip - f * size + s * (side * size);
        const P across = norm(cross(cross(f, s), b - a)) * (thick * 0.5f);   // in the plane, across the stroke
        quad(o, a - across, clear, b - across, clear, b, col, a, col);
        quad(o, a, col, b, col, b + across, clear, a + across, clear);
    }
}

// A camera-facing ribbon along the points, soft-edged across. width(s, u) / colour(s, u): s metres from the
// start, u = s / length.
template <typename W, typename K>
inline void ribbon(std::vector<GuideVertex>& o, const std::vector<P>& pts, P eye, W width, K colour) {
    if (pts.size() < 2) return;
    std::vector<float> s(pts.size(), 0);
    for (size_t i = 1; i < pts.size(); ++i) s[i] = s[i - 1] + std::sqrt(dot(pts[i] - pts[i - 1], pts[i] - pts[i - 1]));
    const float len = std::max(s.back(), 1e-4f);
    P prevL{}, prevM{}, prevR{};
    C prevC{};
    for (size_t i = 0; i < pts.size(); ++i) {
        const P t = norm(pts[std::min(i + 1, pts.size() - 1)] - pts[i > 0 ? i - 1 : 0]);
        const P side = norm(cross(t, eye - pts[i])) * width(s[i], s[i] / len);
        const C c = colour(s[i], s[i] / len);
        const P L = pts[i] - side, M = pts[i], R = pts[i] + side;
        if (i > 0) {
            quad(o, prevL, alpha(prevC, 0), L, alpha(c, 0), M, c, prevM, prevC);
            quad(o, prevM, prevC, M, c, R, alpha(c, 0), prevR, alpha(prevC, 0));
        }
        prevL = L; prevM = M; prevR = R; prevC = c;
    }
}

// The points resampled every `step` metres (dashes need even spacing; the arc's own points are 7 m/s x 30 ms).
inline std::vector<P> resample(const std::vector<P>& in, float step) {
    std::vector<P> out;
    if (in.empty()) return out;
    out.push_back(in[0]);
    float carry = 0;
    for (size_t i = 1; i < in.size(); ++i) {
        const P d = in[i] - in[i - 1];
        const float l = std::sqrt(dot(d, d));
        float at = step - carry;
        for (; at < l; at += step) out.push_back(in[i - 1] + d * (at / l));
        carry = l - (at - step);
    }
    if (std::sqrt(dot(out.back() - in.back(), out.back() - in.back())) > 1e-4f) out.push_back(in.back());
    return out;
}

}  // namespace guide

// Everything in `g` as triangles, for an eye at `eye` (ribbons face it) at time `t` seconds (dashes flow, rings
// pulse). Appends to `out`.
inline void buildGuide(const LocoGuide& g, const float eye[3], float t, std::vector<GuideVertex>& out) {
    using namespace guide;
    const P E{eye[0], eye[1], eye[2]};
    const C cyan{0.30f, 0.78f, 1.00f, 1}, ice{0.86f, 0.97f, 1.00f, 1}, red{1.00f, 0.32f, 0.26f, 1}, gold{1.00f, 0.78f, 0.42f, 1};
    const float appear = smooth(0.0f, 0.12f, g.aimTime);
    const float pulse = 0.5f + 0.5f * std::sin(t * 6.2831853f * 1.1f);   // ~1 Hz: alive, not flashing

    // Seats near you: a faint ring on the seat, a backrest arc behind it and a chevron the way it faces;
    // bright and breathing while pointed at.
    for (const LocoGuide::Seat& s : g.seats) {
        const P c{s.pos[0], s.pos[1] + 0.08f, s.pos[2]};   // a little over the seat point: cushions sit above it
        const P f{-std::sin(s.yaw), 0, -std::cos(s.yaw)}, r{std::cos(s.yaw), 0, -std::sin(s.yaw)}, up{0, 1, 0};
        constexpr float kSize = 0.6f;                      // small: a hint on the seat, not furniture of its own
        const float k = s.hot ? 0.8f + 0.2f * pulse : 0.38f * s.near, grow = (s.hot ? 1.0f + 0.05f * pulse : 1.0f) * kSize;
        const C col = alpha(s.hot ? mix(gold, C{1, 0.95f, 0.85f, 1}, 0.3f) : gold, k);
        ring(out, c, r, f, 0.20f * grow, 0.235f * grow, col);
        if (s.hot) disc(out, c, r, f, 0.20f * grow, alpha(gold, 0.16f), alpha(gold, 0.05f));
        // the backrest: an upright arc behind the seat, so it reads as a chair from any side
        std::vector<P> back;
        for (int i = 0; i <= 16; ++i) {
            const float a = -1.1f + 2.2f * i / 16;
            back.push_back(c - f * (0.235f * grow * std::cos(a)) + r * (0.235f * grow * std::sin(a)) + up * (0.20f * grow * std::cos(a * 0.9f)));
        }
        ribbon(out, back, E, [](float, float) { return 0.012f; }, [&](float, float) { return col; });
        chevron(out, c + f * (0.34f * grow), f, r, 0.07f * grow, 0.022f * grow, col);
        // and a "here" chevron floating over it, above chair backs, so it shows from behind the furniture too;
        // pointed at, it bobs gently (the marker moves, never your view)
        if (s.hot) {                                       // (only while pointed at: close by, the seat's ring says enough)
            const P over = c + up * (0.45f + 0.03f * pulse);
            const P across = norm(cross(up, E - over));
            chevron(out, over, up * -1.0f, across, 0.06f, 0.02f, col);
        }
    }

    // The pointer: from the hand to the seat it's on, fading in from the hand.
    if (g.pointer) {
        const P a{g.pointerFrom[0], g.pointerFrom[1], g.pointerFrom[2]}, b{g.pointerTo[0], g.pointerTo[1], g.pointerTo[2]};
        std::vector<P> line;
        for (int i = 0; i <= 24; ++i) line.push_back(a + (b - a) * (i / 24.0f));
        ribbon(out, line, E, [](float, float u) { return 0.003f + 0.002f * u; },
               [&](float, float u) { return alpha(gold, 0.85f * smooth(0.0f, 0.35f, u)); });
        P u, w;
        basis(norm(E - b), u, w);
        disc(out, b, u, w, 0.018f, alpha(C{1, 0.95f, 0.85f, 1}, 0.9f), alpha(gold, 0), 16);
    }

    if (!g.aiming || g.arc.size() < 6) return;
    std::vector<P> raw;
    for (size_t i = 0; i + 2 < g.arc.size(); i += 3) raw.push_back({g.arc[i], g.arc[i + 1], g.arc[i + 2]});
    const std::vector<P> pts = resample(raw, 0.03f);
    const C tip = g.seat ? gold : ice;
    // The arc: a soft glow under a bright core, both transparent at the hand (so it grows out of the controller
    // rather than starting in your face) and, when it lands, dashes flowing toward the target. Aimed nowhere it is
    // dim red and still.
    auto core = [&](float s, float u) {
        const float hand = smooth(0.0f, 0.35f, s);
        if (!g.valid) return alpha(red, 0.55f * hand * appear * (1 - 0.6f * smooth(0.7f, 1.0f, u)));
        const float d = 0.5f + 0.5f * std::cos(6.2831853f * (s - t * 1.4f) / 0.22f);   // a dash every 22 cm, 1.4 m/s
        const float dash = 0.4f + 0.6f * smooth(0.25f, 0.75f, d);
        return alpha(mix(cyan, tip, smooth(0.2f, 1.0f, u)), 0.95f * hand * dash * appear);
    };
    ribbon(out, pts, E, [](float, float u) { return 0.030f + 0.012f * u; },
           [&](float s, float u) { const C c = core(s, u); return alpha(c, 0.28f); });
    ribbon(out, pts, E, [](float, float u) { return 0.009f + 0.004f * u; }, core);
    if (!g.valid) return;

    // The landing marker, flat on the surface: a pulsing ring, a faint disc, a ripple spreading out every second,
    // and a chevron pointing the way you're going.
    const P n = norm(P{g.normal[0], g.normal[1], g.normal[2]});
    const P c = P{g.target[0], g.target[1], g.target[2]} + n * 0.012f;
    P u, w;
    basis(n, u, w);
    const float R = 0.26f * (1.0f + 0.035f * pulse);
    ring(out, c, u, w, R - 0.022f, R + 0.022f, alpha(tip, 0.9f * appear));
    disc(out, c, u, w, R - 0.02f, alpha(tip, 0.20f * appear), alpha(cyan, 0.08f * appear));
    const float ph = std::fmod(t, 1.0f);
    ring(out, c, u, w, R + 0.1f * ph - 0.012f, R + 0.1f * ph + 0.012f, alpha(tip, 0.5f * (1 - ph) * appear));
    // which way: from the hand toward the target, along the surface
    P f = P{g.target[0], g.target[1], g.target[2]} - raw.front();
    f = f - n * dot(f, n);
    if (dot(f, f) > 1e-6f) {
        f = norm(f);
        chevron(out, c + f * (R - 0.06f), f, cross(n, f), 0.08f, 0.026f, alpha(tip, 0.85f * appear));
    }
}
