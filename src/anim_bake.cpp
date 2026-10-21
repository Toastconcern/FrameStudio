#include "anim_bake.h"
#include "bake_io.h"

#include "loaders/opa_loader.h"
#include "loaders/gltf_loader.h"

#include <algorithm>
#include <climits>
#include <functional>
#include <set>
#include <memory>

namespace {

using bakeio::Reader;
using bakeio::Writer;

constexpr u8 kClay = 1, kGltf = 2;

// Which nodes the animated meshes hang from: those nodes and every ancestor the evaluation reaches from them, the
// rest dropped (kept in their order, so each still follows its parents). `parentOf(i)` is the parent the evaluation
// uses (-1: none); out[i] = the node's new index, -1 when it isn't kept.
template <typename ParentOf>
std::vector<int> keptNodes(size_t n, const std::vector<int>& used, ParentOf parentOf) {
    std::vector<char> keep(n, 0);
    for (int u : used) {
        for (int i = u, steps = 0; i >= 0 && static_cast<size_t>(i) < n && !keep[i] && steps <= static_cast<int>(n); ++steps) {
            keep[i] = 1;
            i = parentOf(i);
        }
    }
    std::vector<int> out(n, -1);
    int k = 0;
    for (size_t i = 0; i < n; ++i) if (keep[i]) out[i] = k++;
    return out;
}

// ── Clay (OpaLoader) ──

void writeTrack(Writer& w, const OpaLoader::Track& t) { w.pod(t.comps); w.pod(t.nFrames); w.pod(t.effFrames); w.vec(t.v); }
OpaLoader::Track readTrack(Reader& r) {
    OpaLoader::Track t;
    t.comps = r.pod<int>(); t.nFrames = r.pod<int>(); t.effFrames = r.pod<int>(); t.v = r.vec<float>();
    return t;
}
// sampleTrack reads nFrames x comps values when the track has frames and its comps are what it asks for (3 or 4)
bool trackOk(const OpaLoader::Track& t) { return t.nFrames <= 0 || (t.comps != 3 && t.comps != 4) || t.v.size() >= static_cast<size_t>(t.nFrames) * t.comps; }

void saveClay(const OpaLoader& L, Writer& w) {
    w.f(L.animFps); w.pod(L.animMaxFrames); w.f(L.vatFps); w.f(L.uvMaxLoopSec);
    // the node hierarchy, as far as the animated meshes need it (evalAnimNodes: a parent at or after its child is none)
    std::vector<int> used;
    for (const auto& ar : L.animRecs) used.push_back(static_cast<int>(ar.nodeIdx));
    const std::vector<int> nodeMap = keptNodes(L.animNodes.size(), used, [&](int i) {
        const int p = L.animNodes[i].parent;
        return p >= 0 && p < i ? p : -1;
    });
    std::vector<const OpaLoader::Node*> nodes;
    for (size_t i = 0; i < L.animNodes.size(); ++i) if (nodeMap[i] >= 0) nodes.push_back(&L.animNodes[i]);
    w.u(static_cast<u32>(nodes.size()));
    for (const OpaLoader::Node* nd : nodes) {
        const int i = static_cast<int>(nd - L.animNodes.data()), p = nd->parent;
        w.str(nd->name); w.pod<int>(p >= 0 && p < i ? nodeMap[p] : -1); w.fa(nd->t, 3); w.fa(nd->r, 4); w.fa(nd->s, 3);
    }
    {   // their keyed tracks (by node name, as evalAnimNodes looks them up)
        std::vector<std::pair<const std::string*, const OpaLoader::NodeTracks*>> tracks;
        std::set<std::string> seen;
        for (const OpaLoader::Node* nd : nodes) {
            const auto it = L.nodeAnim.find(nd->name);
            if (it != L.nodeAnim.end() && seen.insert(nd->name).second) tracks.emplace_back(&it->first, &it->second);
        }
        w.u(static_cast<u32>(tracks.size()));
        for (const auto& t : tracks) { w.str(*t.first); writeTrack(w, t.second->t); writeTrack(w, t.second->r); writeTrack(w, t.second->s); }
    }
    w.u(static_cast<u32>(L.animRecs.size()));
    for (const auto& ar : L.animRecs) {
        w.u(static_cast<u32>(ar.meshIdx));
        w.pod<int>(ar.nodeIdx < nodeMap.size() ? nodeMap[ar.nodeIdx] : INT_MAX);   // (out of range: evaluated as no node)
        w.vec(ar.basePos); w.pod(ar.rigid); w.pod(ar.rest); w.fa(ar.restInv, 16); w.b(ar.atRest);
    }
    // skin clips the skinned meshes play (animate() evaluates every clip each frame: the unused ones go)
    std::vector<int> clipMap(L.clips.size(), -1);
    std::vector<const OpaLoader::AnimClip*> clips;
    for (const auto& sr : L.skinRecs)
        if (sr.clipIdx >= 0 && static_cast<size_t>(sr.clipIdx) < L.clips.size() && clipMap[sr.clipIdx] < 0) clipMap[sr.clipIdx] = 0;
    for (size_t c = 0; c < L.clips.size(); ++c) if (clipMap[c] >= 0) { clipMap[c] = static_cast<int>(clips.size()); clips.push_back(&L.clips[c]); }
    w.u(static_cast<u32>(clips.size()));
    for (const OpaLoader::AnimClip* c : clips) { w.pod(c->numJoints); w.pod(c->numFrames); w.vec(c->parents); w.vec(c->mats); }
    w.u(static_cast<u32>(L.skinRecs.size()));
    for (const auto& sr : L.skinRecs) {
        w.u(static_cast<u32>(sr.meshIdx));
        w.pod<int>(sr.clipIdx >= 0 && static_cast<size_t>(sr.clipIdx) < clipMap.size() ? clipMap[sr.clipIdx] : -1);
        w.pod(sr.nJoints); w.b(sr.gpu);
        // skinned on the GPU: the bind pose and weights are the mesh's own (skinBindPos, boneIndices / boneWeights)
        static const std::vector<float> noF;
        static const std::vector<int> noI;
        w.vec(sr.gpu ? noF : sr.basePos); w.vec(sr.gpu ? noI : sr.jidx); w.vec(sr.gpu ? noF : sr.jw);
        w.vec(sr.boneClip); w.vec(sr.invBind); w.vec(sr.jointBox); w.fa(sr.unriggedBox, 6);
    }
    {   // UV tracks the UV-animated meshes play, each with its scroll-or-flipbook answer
        std::vector<std::pair<const std::string*, const OpaLoader::UVTrack*>> tracks;
        std::set<std::string> seen;
        for (const auto& ur : L.uvAnimRecs) {
            const auto it = L.matUVAnim.find(ur.node);
            if (it != L.matUVAnim.end() && seen.insert(ur.node).second) tracks.emplace_back(&it->first, &it->second);
        }
        w.u(static_cast<u32>(tracks.size()));
        for (const auto& t : tracks) {
            const OpaLoader::UVTrack& tr = *t.second;
            int scroll = tr.scroll;
            float ru = tr.ru, rv = tr.rv;
            if (scroll < 0) scroll = L.uvScrollRate(tr, ru, rv) ? 1 : 0;
            w.str(*t.first); w.pod(tr.nFrames); w.vec(tr.m); w.pod(scroll); w.f(ru); w.f(rv);
        }
    }
    w.u(static_cast<u32>(L.uvAnimRecs.size()));
    for (const auto& ur : L.uvAnimRecs) { w.u(static_cast<u32>(ur.meshIdx)); w.str(ur.node); w.vec(ur.baseUV); w.b(ur.atRest); }
    {
        std::vector<std::pair<const std::string*, const OpaLoader::TintTrack*>> tracks;
        std::set<std::string> seen;
        for (const auto& tr : L.tintRecs) {
            const auto it = L.matTintAnim.find(tr.node);
            if (it != L.matTintAnim.end() && seen.insert(tr.node).second) tracks.emplace_back(&it->first, &it->second);
        }
        w.u(static_cast<u32>(tracks.size()));
        for (const auto& t : tracks) { w.str(*t.first); w.pod(t.second->nFrames); w.vec(t.second->rgba); }
    }
    w.u(static_cast<u32>(L.tintRecs.size()));
    for (const auto& tr : L.tintRecs) { w.u(static_cast<u32>(tr.meshIdx)); w.str(tr.node); }
    std::vector<const OpaLoader::VatData*> vats;
    for (const auto& vr : L.vatRecs) if (vr.vd && std::find(vats.begin(), vats.end(), vr.vd) == vats.end()) vats.push_back(vr.vd);
    w.u(static_cast<u32>(vats.size()));
    for (const OpaLoader::VatData* vd : vats) { w.pod(vd->cols); w.pod(vd->frames); w.vec(vd->off); }
    w.u(static_cast<u32>(L.vatRecs.size()));
    for (const auto& vr : L.vatRecs) {
        w.u(static_cast<u32>(vr.meshIdx)); w.vec(vr.basePos); w.vec(vr.col); w.pod(vr.world);
        w.pod<int>(vr.vd ? static_cast<int>(std::find(vats.begin(), vats.end(), vr.vd) - vats.begin()) : -1);
    }
}

std::shared_ptr<OpaLoader> loadClay(Reader& r, size_t meshes) {
    auto L = std::make_shared<OpaLoader>();
    L->verbose = false;
    L->animFps = r.f(); L->animMaxFrames = r.pod<int>(); L->vatFps = r.f(); L->uvMaxLoopSec = r.f();
    L->animNodes.resize(r.count(44));
    for (auto& nd : L->animNodes) { if (r.bad) return nullptr; nd.name = r.str(); nd.parent = r.pod<int>(); r.fa(nd.t, 3); r.fa(nd.r, 4); r.fa(nd.s, 3); }
    for (u32 n = r.count(40); n-- && !r.bad;) {
        const std::string name = r.str();
        OpaLoader::NodeTracks nt;
        nt.t = readTrack(r); nt.r = readTrack(r); nt.s = readTrack(r);
        if (!trackOk(nt.t) || !trackOk(nt.r) || !trackOk(nt.s)) return nullptr;
        L->nodeAnim[name] = std::move(nt);
    }
    L->animRecs.resize(r.count(145));
    for (auto& ar : L->animRecs) {
        if (r.bad) return nullptr;
        ar.meshIdx = r.u(); ar.nodeIdx = static_cast<u32>(r.pod<int>()); ar.basePos = r.vec<float>(); ar.rigid = r.pod<int>();
        ar.rest = r.pod<OpaLoader::Mat4>(); r.fa(ar.restInv, 16); ar.atRest = r.b();
        ar.parentWorld = OpaLoader::identity();
        if (ar.meshIdx >= meshes) return nullptr;
    }
    L->clips.resize(r.count(16));
    for (auto& c : L->clips) {
        if (r.bad) return nullptr;
        c.numJoints = r.pod<int>(); c.numFrames = r.pod<int>(); c.parents = r.vec<int>(); c.mats = r.vec<float>();
        if (c.numJoints < 0 || c.numFrames < 0 || c.mats.size() < static_cast<size_t>(c.numFrames) * c.numJoints * 16) return nullptr;
    }
    L->skinRecs.resize(r.count(57));
    for (auto& sr : L->skinRecs) {
        if (r.bad) return nullptr;
        sr.meshIdx = r.u(); sr.clipIdx = r.pod<int>(); sr.nJoints = r.pod<int>(); sr.gpu = r.b();
        sr.basePos = r.vec<float>(); sr.jidx = r.vec<int>(); sr.jw = r.vec<float>();
        sr.boneClip = r.vec<int>(); sr.invBind = r.vec<float>(); sr.jointBox = r.vec<float>(); r.fa(sr.unriggedBox, 6);
        const size_t nv = sr.basePos.size() / 3;
        if (sr.meshIdx >= meshes || (!sr.gpu && (sr.jidx.size() < nv * 4 || sr.jw.size() < nv * 4)) ||
            (sr.gpu && sr.jointBox.size() < static_cast<size_t>(std::max(0, sr.nJoints)) * 6))
            return nullptr;
    }
    for (u32 n = r.count(24); n-- && !r.bad;) {
        const std::string name = r.str();
        OpaLoader::UVTrack tr;
        tr.nFrames = r.pod<int>(); tr.m = r.vec<float>(); tr.scroll = r.pod<int>(); tr.ru = r.f(); tr.rv = r.f();
        if (tr.nFrames < 0 || tr.m.size() < static_cast<size_t>(tr.nFrames) * 6) return nullptr;
        L->matUVAnim[name] = std::move(tr);
    }
    L->uvAnimRecs.resize(r.count(13));
    for (auto& ur : L->uvAnimRecs) {
        if (r.bad) return nullptr;
        ur.meshIdx = r.u(); ur.node = r.str(); ur.baseUV = r.vec<float>(); ur.atRest = r.b();
        if (ur.meshIdx >= meshes) return nullptr;
    }
    for (u32 n = r.count(12); n-- && !r.bad;) {
        const std::string name = r.str();
        OpaLoader::TintTrack tt;
        tt.nFrames = r.pod<int>(); tt.rgba = r.vec<float>();
        if (tt.nFrames < 0 || tt.rgba.size() < static_cast<size_t>(tt.nFrames) * 4) return nullptr;
        L->matTintAnim[name] = std::move(tt);
    }
    L->tintRecs.resize(r.count(8));
    for (auto& tr : L->tintRecs) { if (r.bad) return nullptr; tr.meshIdx = r.u(); tr.node = r.str(); if (tr.meshIdx >= meshes) return nullptr; }
    std::vector<std::string> vatNames(r.count(12));
    for (size_t i = 0; i < vatNames.size() && !r.bad; ++i) {
        vatNames[i] = std::to_string(i);   // (keyed by the geo name when parsed; by order here)
        OpaLoader::VatData& vd = L->vatByBase[vatNames[i]];
        vd.cols = r.pod<int>(); vd.frames = r.pod<int>(); vd.off = r.vec<float>();
        if (vd.cols < 0 || vd.frames < 0 || vd.off.size() < static_cast<size_t>(vd.frames) * vd.cols * 3) return nullptr;
    }
    L->vatRecs.resize(r.count(80));
    for (auto& vr : L->vatRecs) {
        if (r.bad) return nullptr;
        vr.meshIdx = r.u(); vr.basePos = r.vec<float>(); vr.col = r.vec<int>(); vr.world = r.pod<OpaLoader::Mat4>();
        const int vd = r.pod<int>();
        vr.vd = vd >= 0 && static_cast<size_t>(vd) < vatNames.size() ? &L->vatByBase[vatNames[vd]] : nullptr;
        if (vr.meshIdx >= meshes || (vr.vd && vr.col.size() < vr.basePos.size() / 3)) return nullptr;
    }
    return r.bad ? nullptr : L;
}

// ── glTF (GltfLoader) ──

void saveGltf(const GltfLoader& L, Writer& w) {
    w.f(L.animDuration); w.fa(L.reanchor, 3);
    // the nodes the animated and skinned meshes follow, with their ancestors (nodeWorlds: an out-of-range parent is
    // evaluated as the identity, so it stays out of range)
    std::vector<int> used;
    for (const auto& rec : L.nodeAnimRecs) used.push_back(rec.nodeIdx);
    for (const auto& rec : L.skinned)
        if (rec.skin >= 0 && static_cast<size_t>(rec.skin) < L.gskins.size()) for (int j : L.gskins[rec.skin].joints) used.push_back(j);
    const size_t nn = L.gnodes.size();
    const std::vector<int> nodeMap = keptNodes(nn, used, [&](int i) { return L.gnodes[i].parent; });
    auto mapNode = [&](int n) { return n >= 0 && static_cast<size_t>(n) < nn ? (nodeMap[n] >= 0 ? nodeMap[n] : INT_MAX) : (n < 0 ? n : INT_MAX); };
    u32 kept = 0;
    for (int m : nodeMap) kept += m >= 0;
    w.u(kept);
    for (size_t i = 0; i < nn; ++i) {
        if (nodeMap[i] < 0) continue;
        const GltfLoader::GNode& g = L.gnodes[i];
        w.fa(g.t, 3); w.fa(g.r, 4); w.fa(g.s, 3); w.pod<int>(mapNode(g.parent));
    }
    w.u(static_cast<u32>(L.gskins.size()));
    for (const auto& sk : L.gskins) {
        std::vector<int> joints;
        for (int j : sk.joints) joints.push_back(mapNode(j));
        w.vec(joints); w.vec(sk.ibm);
    }
    // the channels on kept nodes (in their order: a later one on the same node wins), and the samplers they read
    std::vector<int> samplerMap(L.gsamplers.size(), -1);
    std::vector<const GltfLoader::GChannel*> channels;
    for (const auto& ch : L.gchannels)
        if (ch.node >= 0 && static_cast<size_t>(ch.node) < nn && nodeMap[ch.node] >= 0 && ch.sampler >= 0 && static_cast<size_t>(ch.sampler) < L.gsamplers.size()) {
            channels.push_back(&ch);
            samplerMap[ch.sampler] = 0;
        }
    std::vector<const GltfLoader::GSampler*> samplers;
    for (size_t s = 0; s < L.gsamplers.size(); ++s) if (samplerMap[s] >= 0) { samplerMap[s] = static_cast<int>(samplers.size()); samplers.push_back(&L.gsamplers[s]); }
    w.u(static_cast<u32>(samplers.size()));
    for (const GltfLoader::GSampler* s : samplers) { w.vec(s->times); w.vec(s->vals); w.pod(s->comps); w.b(s->step); }
    w.u(static_cast<u32>(channels.size()));
    for (const GltfLoader::GChannel* ch : channels) { w.pod<int>(nodeMap[ch->node]); w.pod(ch->path); w.pod<int>(samplerMap[ch->sampler]); }
    w.u(static_cast<u32>(L.skinned.size()));
    for (const auto& rec : L.skinned) { w.pod(rec.meshIdx); w.pod(rec.skin); w.u(rec.nv); w.vec(rec.basePos); w.vec(rec.jidx); w.vec(rec.jw); }
    w.u(static_cast<u32>(L.nodeAnimRecs.size()));
    for (const auto& rec : L.nodeAnimRecs) { w.pod(rec.meshIdx); w.pod<int>(mapNode(rec.nodeIdx)); w.vec(rec.basePos); w.pod(rec.rigid); w.fa(rec.rest, 16); w.fa(rec.restInv, 16); w.b(rec.atRest); }
}

std::shared_ptr<GltfLoader> loadGltf(Reader& r, size_t meshes) {
    auto L = std::make_shared<GltfLoader>();
    L->verbose = false;
    L->animDuration = r.f(); r.fa(L->reanchor, 3);
    L->gnodes.resize(r.count(44));
    for (auto& g : L->gnodes) { if (r.bad) return nullptr; r.fa(g.t, 3); r.fa(g.r, 4); r.fa(g.s, 3); g.parent = r.pod<int>(); }
    L->gskins.resize(r.count(8));
    for (auto& sk : L->gskins) { if (r.bad) return nullptr; sk.joints = r.vec<int>(); sk.ibm = r.vec<float>(); }
    L->gsamplers.resize(r.count(13));
    for (auto& s : L->gsamplers) {
        if (r.bad) return nullptr;
        s.times = r.vec<float>(); s.vals = r.vec<float>(); s.comps = r.pod<int>(); s.step = r.b();
        // (sampleSampler: up to 4 values a key, a key's worth even without keys)
        if (s.comps < 0 || s.comps > 4 || (s.times.empty() ? !s.vals.empty() && s.vals.size() < static_cast<size_t>(s.comps)
                                                            : s.vals.size() < s.times.size() * s.comps))
            return nullptr;
    }
    L->gchannels.resize(r.count(12));
    for (auto& ch : L->gchannels) { if (r.bad) return nullptr; ch.node = r.pod<int>(); ch.path = r.pod<int>(); ch.sampler = r.pod<int>(); }
    L->skinned.resize(r.count(24));
    for (auto& rec : L->skinned) {
        if (r.bad) return nullptr;
        rec.meshIdx = r.pod<int>(); rec.skin = r.pod<int>(); rec.nv = r.u(); rec.basePos = r.vec<float>(); rec.jidx = r.vec<u8>(); rec.jw = r.vec<float>();
        if (rec.meshIdx < 0 || static_cast<size_t>(rec.meshIdx) >= meshes || rec.basePos.size() < static_cast<size_t>(rec.nv) * 3 ||
            rec.jidx.size() < static_cast<size_t>(rec.nv) * 4 || rec.jw.size() < static_cast<size_t>(rec.nv) * 4)
            return nullptr;
    }
    L->nodeAnimRecs.resize(r.count(145));
    for (auto& rec : L->nodeAnimRecs) {
        if (r.bad) return nullptr;
        rec.meshIdx = r.pod<int>(); rec.nodeIdx = r.pod<int>(); rec.basePos = r.vec<float>(); rec.rigid = r.pod<int>(); r.fa(rec.rest, 16); r.fa(rec.restInv, 16); rec.atRest = r.b();
        if (rec.meshIdx < 0 || static_cast<size_t>(rec.meshIdx) >= meshes) return nullptr;
    }
    return r.bad ? nullptr : L;
}

// The per-frame play of a loader's animation, parsed or rebuilt: it works on the scene's meshes (the loader indexes them
// as its own, same order).
template <typename Loader> std::function<void(float, std::vector<MeshData>&)> player(std::shared_ptr<Loader> L) {
    return [L](float t, std::vector<MeshData>& m) { std::swap(L->meshes, m); L->animateRuntime(t); std::swap(L->meshes, m); };
}

// One stored animation, rebuilt: the loader holding it.
struct Animator {
    std::shared_ptr<OpaLoader> clay;
    std::shared_ptr<GltfLoader> gltf;
    std::function<void(float, std::vector<MeshData>&)> animate() const { return clay ? player(clay) : gltf ? player(gltf) : nullptr; }
    void save(std::vector<u8>& out) const { if (clay) animbake::save(*clay, out); else if (gltf) animbake::save(*gltf, out); }
};

bool rebuild(const std::vector<u8>& bytes, size_t meshes, Animator& a) {
    Reader r{bytes.data(), bytes.data() + bytes.size()};
    const u8 kind = r.pod<u8>();
    a = Animator();
    if (kind == kClay) a.clay = loadClay(r, meshes);
    else if (kind == kGltf) a.gltf = loadGltf(r, meshes);
    return (a.clay || a.gltf) && !r.bad && r.p == r.end;
}

}  // namespace

namespace animbake {

void adopt(HslScene& scene, std::shared_ptr<OpaLoader> loader) {
    scene.animate = player(loader);
    scene.saveAnimation = [loader](std::vector<u8>& out) { save(*loader, out); };
}

void adopt(HslScene& scene, std::shared_ptr<GltfLoader> loader) {
    scene.animate = player(loader);
    scene.saveAnimation = [loader](std::vector<u8>& out) { save(*loader, out); };
}

void save(const OpaLoader& loader, std::vector<u8>& out) {
    out.clear();
    Writer w{out};
    w.pod(kClay);
    saveClay(loader, w);
}

void save(const GltfLoader& loader, std::vector<u8>& out) {
    out.clear();
    Writer w{out};
    w.pod(kGltf);
    saveGltf(loader, w);
}

bool load(const std::vector<u8>& bytes, size_t meshes, HslScene& scene) {
    Animator a;
    if (!rebuild(bytes, meshes, a)) return false;
    scene.animate = a.animate();
    scene.saveAnimation = [bytes](std::vector<u8>& out) { out = bytes; };
    return true;
}

std::vector<int> targets(const std::vector<u8>& bytes) {
    Animator a;
    std::vector<int> out;
    if (!rebuild(bytes, SIZE_MAX, a)) return out;
    if (a.clay) {
        const OpaLoader& L = *a.clay;
        for (const auto& x : L.animRecs) out.push_back(static_cast<int>(x.meshIdx));
        for (const auto& x : L.skinRecs) out.push_back(static_cast<int>(x.meshIdx));
        for (const auto& x : L.uvAnimRecs) out.push_back(static_cast<int>(x.meshIdx));
        for (const auto& x : L.tintRecs) out.push_back(static_cast<int>(x.meshIdx));
        for (const auto& x : L.vatRecs) out.push_back(static_cast<int>(x.meshIdx));
    } else {
        for (const auto& x : a.gltf->skinned) out.push_back(x.meshIdx);
        for (const auto& x : a.gltf->nodeAnimRecs) out.push_back(x.meshIdx);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool startPose(std::vector<u8>& bytes, const std::vector<MeshData>& meshes, std::vector<std::pair<size_t, MeshData>>& posed) {
    posed.clear();
    Animator a;
    if (!rebuild(bytes, meshes.size(), a)) return false;
    // only the meshes it writes are copied; the others stay empty (animate() never looks at them)
    std::vector<MeshData> work(meshes.size());
    const std::vector<int> written = targets(bytes);
    for (int m : written) work[m] = meshes[m];
    a.animate()(0.0f, work);
    for (int m : written) posed.emplace_back(static_cast<size_t>(m), std::move(work[m]));
    a.save(bytes);
    return true;
}

bool toStart(HslScene& scene) {
    if (!scene.saveAnimation) return false;
    std::vector<u8> bytes;
    scene.saveAnimation(bytes);
    std::vector<std::pair<size_t, MeshData>> posed;
    if (!startPose(bytes, scene.meshes, posed)) return false;
    for (auto& [m, md] : posed) scene.meshes[m] = std::move(md);
    return true;
}

}  // namespace animbake
