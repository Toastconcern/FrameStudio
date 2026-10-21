#pragma once
#include "core/types.h"
#include "core/tinyjson.h"
#include <string>
#include <vector>
#include <cmath>

// HSTF v5 JSON template parser — 1:1 replica of
// arvr/projects/mhe/frameworks/template/code/source/template/asset/TemplateAssetHandler.cpp
//
// Parses Horizon Scene Template Format JSON, extracting DrawableEntity list.

// Flatten a component's `data` JSON to (dotted-key, display-string) pairs so the editor can inspect/edit ANY
// component generically (nested objects -> "a.b.x", arrays -> "a[0].x"). Truth for field meaning: V203_COMPONENTS_IDA.md.
inline void flattenHstfJson(const tinyjson::Value& v, const std::string& prefix,
                            std::vector<std::pair<std::string,std::string>>& out, int depth = 0) {
    if (depth > 8) return;
    if (v.isObject()) {
        for (const auto& kv : v.objVal) {
            std::string key = prefix.empty() ? kv.first : prefix + "." + kv.first;
            flattenHstfJson(kv.second, key, out, depth + 1);
        }
    } else if (v.isArray()) {
        for (size_t i = 0; i < v.arrVal.size(); ++i)
            flattenHstfJson(v.arrVal[i], prefix + "[" + std::to_string(i) + "]", out, depth + 1);
    } else {
        char buf[64]; std::string s;
        if (v.isBool())        s = v.asBool() ? "true" : "false";
        else if (v.isInt())  { snprintf(buf, sizeof buf, "%lld", (long long)v.asInt()); s = buf; }
        else if (v.isFloat()){ snprintf(buf, sizeof buf, "%g", v.asFloat()); s = buf; }
        else if (v.isString()) s = v.asString();
        else s = "null";
        out.push_back({ prefix, s });
    }
}

// Capture EVERY component on an entity (class/version/flattened fields) for the generic editor inspector — even
// components the typed render pipeline ignores (sound, locomotion, lightprobes, sittable, colliders, extras...).
inline void captureHstfComponents(const tinyjson::Value& ent, DrawableEntity& draw) {
    if (!ent.has("components") || !ent["components"].isArray()) return;
    const auto& comps = ent["components"];
    for (size_t ci = 0; ci < comps.size(); ++ci) {
        const auto& comp = comps[ci];
        if (!comp.has("data")) continue;
        const auto& cd = comp["data"];
        EnvComponent ec;
        ec.cls = cd.has("class") ? cd["class"].asString() : "";
        size_t p = ec.cls.rfind("::");
        ec.shortCls = (p == std::string::npos) ? ec.cls : ec.cls.substr(p + 2);
        ec.version = cd.has("version") ? (int)cd["version"].asInt() : 0;
        if (cd.has("data")) flattenHstfJson(cd["data"], "", ec.fields);
        if (std::getenv("HSR_COMPDUMP"))
            fprintf(stderr, "[COMP] %-40s v%d  %zu fields  (entity '%s')\n",
                    ec.shortCls.c_str(), ec.version, ec.fields.size(), draw.name.c_str());
        draw.components.push_back(std::move(ec));
    }
}

// Extract an entity's local TransformPlatformComponent (pos / rot[euler→quat or quat] / scale).

// localRotation without a w: euler radians -> quaternion (x, y, z, w), applied Z first, then X, then Y (q = qy*qx*qz,
// Unity's order). Verified against the device's own baked output: Desert Terrace's balloon lands exactly where the
// Clay build bakes it, and all 13 of Calming's waterfall cards stand upright (texture rows top-down, UV v down).
// The previous ZYX (q = qz*qy*qx) agreed on yaw-only objects but hung the hero waterfall's painting upside down
// and scrolled its overlays upward.
inline void hstfEulerToQuat(float rx, float ry, float rz, float* q) {
    const float cx = std::cos(rx * 0.5f), sx = std::sin(rx * 0.5f), cy = std::cos(ry * 0.5f), sy = std::sin(ry * 0.5f),
                cz = std::cos(rz * 0.5f), sz = std::sin(rz * 0.5f);
    q[0] = cy * sx * cz + sy * cx * sz;
    q[1] = sy * cx * cz - cy * sx * sz;
    q[2] = cy * cx * sz - sy * sx * cz;
    q[3] = cy * cx * cz + sy * sx * sz;
}

inline Transform parseHstfEntityTransform(const tinyjson::Value& ent) {
    Transform t;
    if (!ent.has("components") || !ent["components"].isArray()) return t;
    const auto& comps = ent["components"];
    for (size_t ci = 0; ci < comps.size(); ++ci) {
        const auto& comp = comps[ci];
        if (!comp.has("data")) continue;
        const auto& cd = comp["data"];
        std::string cls = cd.has("class") ? cd["class"].asString() : "";
        if (cls.find("TransformPlatformComponent") == std::string::npos) continue;
        const auto& dat = cd.has("data") ? cd["data"] : tinyjson::Value();
        if (dat.has("localPosition")) {
            const auto& p = dat["localPosition"];
            t.pos[0]=(float)(p.has("x")?p["x"].asFloat():0); t.pos[1]=(float)(p.has("y")?p["y"].asFloat():0); t.pos[2]=(float)(p.has("z")?p["z"].asFloat():0);
        }
        if (dat.has("localRotationQuat")) {
            // AUTHORITATIVE rotation = the cooked quaternion. The sibling localRotation (euler) DISAGREES
            // (e.g. euler=180°-about-Y while the quat is identity) — euler is a stale/display value whose
            // axis order doesn't match. Reading euler flipped/tilted every rotated mesh (slanted sand, gaps).
            const auto& q = dat["localRotationQuat"];
            float qx=(float)(q.has("x")?q["x"].asFloat():0), qy=(float)(q.has("y")?q["y"].asFloat():0), qz=(float)(q.has("z")?q["z"].asFloat():0), qw=(float)(q.has("w")?q["w"].asFloat():1);
            float L=std::sqrt(qx*qx+qy*qy+qz*qz+qw*qw); if(L<1e-8f){qx=qy=qz=0;qw=1;L=1;}
            t.rot[0]=qx/L; t.rot[1]=qy/L; t.rot[2]=qz/L; t.rot[3]=qw/L;
        }
        else if (dat.has("localRotation")) {
            const auto& lr = dat["localRotation"];
            float rx=(float)(lr.has("x")?lr["x"].asFloat():0), ry=(float)(lr.has("y")?lr["y"].asFloat():0), rz=(float)(lr.has("z")?lr["z"].asFloat():0);
            // localRotation: WITH w = quaternion; WITHOUT w = EULER RADIANS (libshell sub_C3B280 = half-angle
            // euler→quat, confirmed in IDA — NOT a quat with derived w).
            if (lr.has("w")) { float rw=(float)lr["w"].asFloat(); float L=std::sqrt(rx*rx+ry*ry+rz*rz+rw*rw); if(L<1e-8f){rx=ry=rz=0;rw=1;L=1;} t.rot[0]=rx/L;t.rot[1]=ry/L;t.rot[2]=rz/L;t.rot[3]=rw/L; }
            else hstfEulerToQuat(rx, ry, rz, t.rot);
        }
        if (dat.has("localScale")) {
            const auto& s = dat["localScale"];
            t.scale[0]=(float)(s.has("x")?s["x"].asFloat():1); t.scale[1]=(float)(s.has("y")?s["y"].asFloat():1); t.scale[2]=(float)(s.has("z")?s["z"].asFloat():1);
        }
    }
    return t;
}

// World transform of a child under a parent: world = parent ∘ child (scale·rotate·translate).
inline Transform composeTransform(const Transform& P, const Transform& C) {
    Transform R;
    R.scale[0]=P.scale[0]*C.scale[0]; R.scale[1]=P.scale[1]*C.scale[1]; R.scale[2]=P.scale[2]*C.scale[2];
    float px=P.rot[0],py=P.rot[1],pz=P.rot[2],pw=P.rot[3];
    float cx=C.rot[0],cy=C.rot[1],cz=C.rot[2],cw=C.rot[3];
    R.rot[0]=pw*cx+px*cw+py*cz-pz*cy;  R.rot[1]=pw*cy-px*cz+py*cw+pz*cx;
    R.rot[2]=pw*cz+px*cy-py*cx+pz*cw;  R.rot[3]=pw*cw-px*cx-py*cy-pz*cz;
    float vx=P.scale[0]*C.pos[0], vy=P.scale[1]*C.pos[1], vz=P.scale[2]*C.pos[2];
    float tx=2*(py*vz-pz*vy), ty=2*(pz*vx-px*vz), tz=2*(px*vy-py*vx);
    R.pos[0]=P.pos[0]+vx+pw*tx+(py*tz-pz*ty);
    R.pos[1]=P.pos[1]+vy+pw*ty+(pz*tx-px*tz);
    R.pos[2]=P.pos[2]+vz+pw*tz+(px*ty-py*tx);
    return R;
}

// ── Template instance deltas ────────────────────────────────────────────────────────────────────
// A file that places a template can change the template's entities: each delta {sourceId, components} targets
// one entity (by id; the first 13 GUID characters identify it) and overrides its components field by field —
// material swaps, moves, isVisibleSelf:false, lightmap params, material property overrides. Ordered: later
// entries (deltas authored further out) win.
using HstfDeltas = std::vector<std::pair<std::string, tinyjson::Value>>;

inline std::string hstfGuid13(const std::string& s) {
    const size_t sl = s.find_last_of('/');
    const std::string g = sl == std::string::npos ? s : s.substr(sl + 1);
    return g.substr(0, 13);
}

// Merge one delta's components into an entity: same class -> the delta's data fields replace the entity's;
// a class the entity lacks is added.
inline void mergeHstfDelta(tinyjson::Value& ent, const tinyjson::Value& delta) {
    // {"sourceId": X, "components": null} REMOVES the entity (HSR Studio: node.removed / draw.removedByDelta).
    if (delta.has("components") && delta["components"].isNull()) {
        tinyjson::Value t(tinyjson::Type::Bool);
        t.boolVal = true;
        ent["__removedByDelta"] = t;
        return;
    }
    if (!delta.has("components") || !delta["components"].isArray()) return;
    if (!ent.has("components") || !ent["components"].isArray()) ent["components"] = tinyjson::Value(tinyjson::Type::Array);
    auto& comps = ent["components"].arrVal;
    for (const auto& dc : delta["components"].arrVal) {
        if (!dc.has("data") || !dc["data"].has("class")) continue;
        const std::string cls = dc["data"]["class"].asString();
        tinyjson::Value* target = nullptr;
        for (auto& c : comps)
            if (c.has("data") && c["data"].has("class") && c["data"]["class"].asString() == cls) { target = &c; break; }
        if (!target) { comps.push_back(dc); continue; }
        const tinyjson::Value& src = dc["data"].has("data") ? dc["data"]["data"] : tinyjson::Value();
        tinyjson::Value& dst = (*target)["data"]["data"];
        if (src.isObject()) {
            if (!dst.isObject()) dst = tinyjson::Value(tinyjson::Type::Object);
            for (const auto& kv : src.objVal) dst.objVal[kv.first] = kv.second;
        }
    }
}

inline void applyHstfDeltas(tinyjson::Value& doc, const HstfDeltas& deltas) {
    if (deltas.empty() || !doc.has("entities") || !doc["entities"].isArray()) return;
    for (auto& e : doc["entities"].arrVal) {
        if (!e.has("id")) continue;
        const std::string g = hstfGuid13(e["id"].asString());
        for (const auto& d : deltas)
            if (d.first == g) mergeHstfDelta(e, d.second);
    }
}

// sceneRefl: a SkyboxPlatformComponent's reflectionMap when the component has no sky mesh of its own (Cascadia's Root
// carries only reflectionMap + diffuseMap; the dome is a separate mesh).
inline bool parseHstf(const std::string& jsonStr, std::vector<DrawableEntity>& out,
                      bool verbose = true, const HstfDeltas* deltas = nullptr, AssetKey* sceneRefl = nullptr) {
    try {
        auto doc = tinyjson::parse(jsonStr);
        if (deltas) applyHstfDeltas(doc, *deltas);
        if (!doc.isObject() || !doc.has("entities")) {
            if (verbose) fprintf(stderr, "[HSTF] ERROR: missing 'entities' array in JSON\n");
            return false;
        }

        const auto& entities = doc["entities"];
        if (!entities.isArray()) {
            if (verbose) fprintf(stderr, "[HSTF] ERROR: 'entities' is not an array\n");
            return false;
        }
        if (verbose) fprintf(stderr, "[HSTF] Parsing %zu entities...\n", entities.size());

        // Index entities by id
        std::map<std::string, tinyjson::Value> entityMap;
        for (size_t i = 0; i < entities.size(); ++i) {
            const auto& e = entities[i];
            if (e.has("id")) entityMap[e["id"].asString()] = e;
        }

        // Where each entity sits in the file: its parents (RelationChildOf), outermost first.
        std::map<std::string, std::string> parentOf;
        if (doc.has("relationships") && doc["relationships"].isArray())
            for (const auto& r : doc["relationships"].arrVal)
                if (r["relationshipType"].asString() == "RelationChildOf") parentOf[r["source"].asString()] = r["destination"].asString();
        auto pathOf = [&](const std::string& id) {
            std::string path;
            auto p = parentOf.find(id);
            for (int guard = 0; p != parentOf.end() && guard < 64; ++guard) {
                auto e = entityMap.find(p->second);
                const std::string n = e != entityMap.end() && e->second.has("name") ? e->second["name"].asString() : p->second;
                path = path.empty() ? n : n + "/" + path;
                p = parentOf.find(p->second);
            }
            return path;
        };
        for (const auto& [eid, ent] : entityMap) {
            if (ent.has("__removedByDelta")) continue;      // removed by a delta: not part of the world
            DrawableEntity draw;
            draw.name = ent.has("name") ? ent["name"].asString() : eid;
            draw.path = pathOf(eid);
            if (verbose) fprintf(stderr, "[HSTF]   Entity '%s' (id=%s)\n", draw.name.c_str(), eid.c_str());
            captureHstfComponents(ent, draw);   // generic capture of ALL components for the editor inspector

            bool hasMesh = false;
            const auto& comps = ent.has("components") ? ent["components"] : tinyjson::Value();
            if (comps.isArray()) {
                for (size_t ci = 0; ci < comps.size(); ++ci) {
                    const auto& comp = comps[ci];
                    if (!comp.has("data")) continue;
                    const auto& cd = comp["data"];
                    std::string cls = cd.has("class") ? cd["class"].asString() : "";
                    const auto& dat = cd.has("data") ? cd["data"] : tinyjson::Value();

                    if (cls.find("TransformPlatformComponent") != std::string::npos) {
                        // A TransformPlatformComponent with NO local pos/rot/scale (data:{}) is the cook
                        // anomaly (e.g. oceanarium Root_sand_SHELL): a flat read leaves it at the origin.
                        // Flagged here so the loader can resolve it from the level's group offset.
                        if (!dat.has("localPosition") && !dat.has("localRotation") &&
                            !dat.has("localRotationQuat") && !dat.has("localScale"))
                            draw.emptyTransform = true;
                        if (dat.has("localPosition")) {
                            draw.transform.pos[0] = (float)(dat["localPosition"].has("x") ? dat["localPosition"]["x"].asFloat() : 0);
                            draw.transform.pos[1] = (float)(dat["localPosition"].has("y") ? dat["localPosition"]["y"].asFloat() : 0);
                            draw.transform.pos[2] = (float)(dat["localPosition"].has("z") ? dat["localPosition"]["z"].asFloat() : 0);
                        }
                        if (dat.has("localRotationQuat")) {
                            // AUTHORITATIVE rotation = the cooked quaternion. The sibling localRotation (euler)
                            // DISAGREES with it (e.g. euler says 180°-about-Y while the quat is identity) — the
                            // euler is a stale/display value whose axis order doesn't match ours. Reading it
                            // flipped/tilted every rotated mesh (slanted sand, "gaps"). Use the quat directly.
                            const auto& q = dat["localRotationQuat"];
                            float qx=(float)(q.has("x")?q["x"].asFloat():0), qy=(float)(q.has("y")?q["y"].asFloat():0);
                            float qz=(float)(q.has("z")?q["z"].asFloat():0), qw=(float)(q.has("w")?q["w"].asFloat():1);
                            float L=std::sqrt(qx*qx+qy*qy+qz*qz+qw*qw); if(L<1e-8f){qx=qy=qz=0;qw=1;L=1;}
                            draw.transform.rot[0]=qx/L; draw.transform.rot[1]=qy/L; draw.transform.rot[2]=qz/L; draw.transform.rot[3]=qw/L;
                        }
                        else if (dat.has("localRotation")) {
                            const auto& lr = dat["localRotation"];
                            float rx = (float)(lr.has("x") ? lr["x"].asFloat() : 0);
                            float ry = (float)(lr.has("y") ? lr["y"].asFloat() : 0);
                            float rz = (float)(lr.has("z") ? lr["z"].asFloat() : 0);
                            if (lr.has("w")) {
                                // Already a quaternion — normalize defensively (buildModelMatrix assumes unit).
                                float rw = (float)lr["w"].asFloat();
                                float L = std::sqrt(rx*rx + ry*ry + rz*rz + rw*rw);
                                if (L < 1e-8f) { rx = ry = rz = 0; rw = 1; L = 1; }
                                draw.transform.rot[0]=rx/L; draw.transform.rot[1]=ry/L;
                                draw.transform.rot[2]=rz/L; draw.transform.rot[3]=rw/L;
                            } else {
                                hstfEulerToQuat(rx, ry, rz, draw.transform.rot);   // localRotation WITHOUT a w = euler radians
                            }
                        }
                        if (dat.has("localScale")) {
                            draw.transform.scale[0] = (float)(dat["localScale"].has("x") ? dat["localScale"]["x"].asFloat() : 1);
                            draw.transform.scale[1] = (float)(dat["localScale"].has("y") ? dat["localScale"]["y"].asFloat() : 1);
                            draw.transform.scale[2] = (float)(dat["localScale"].has("z") ? dat["localScale"]["z"].asFloat() : 1);
                        }
                    }
                    else if (cls == "horizon::platform_api::MeshPlatformComponent") {
                        // Only exact MeshPlatformComponent — not ColliderMeshPlatformComponent etc.
                        // Try "mesh" key (standard) then "meshAsset" (some APK versions).
                        const tinyjson::Value* meshNode = nullptr;
                        if (dat.has("mesh")) meshNode = &dat["mesh"];
                        else if (dat.has("meshAsset")) meshNode = &dat["meshAsset"];
                        if (meshNode) {
                            const auto& m = *meshNode;
                            draw.meshRef = {
                                (u64)(m.has("packageOrRemoteId") ? std::stoull(m["packageOrRemoteId"].asString()) : 0),
                                (u64)(m.has("ingestionId")      ? std::stoull(m["ingestionId"].asString()) : 0),
                                (u32)(m.has("targetId")         ? (u32)m["targetId"].asInt() : 0)
                            };
                            // isVisibleSelf:false (often set by a delta) = not drawn on the device.
                            const bool hiddenSelf = dat.has("isVisibleSelf") && dat["isVisibleSelf"].isBool() &&
                                                    !dat["isVisibleSelf"].boolVal;
                            hasMesh = draw.meshRef.ing != 0 && !hiddenSelf;
                            if (verbose && hasMesh)
                                fprintf(stderr, "[HSTF]     mesh: pkg=%016llX ing=%016llX tgt=%08X\n",
                                    (unsigned long long)draw.meshRef.pkg,
                                    (unsigned long long)draw.meshRef.ing,
                                    draw.meshRef.tgt);
                        }
                    }
                    else if (cls.find("MaterialPlatformComponent") != std::string::npos) {
                        if (dat.has("materials") && dat["materials"].isArray()) {
                            for (size_t mi = 0; mi < dat["materials"].size(); ++mi) {
                                const auto& m = dat["materials"][mi];
                                AssetKey matRef = {
                                    (u64)(m.has("packageOrRemoteId") ? std::stoull(m["packageOrRemoteId"].asString()) : 0),
                                    (u64)(m.has("ingestionId")      ? std::stoull(m["ingestionId"].asString()) : 0),
                                    (u32)(m.has("targetId")         ? (u32)m["targetId"].asInt() : 0)
                                };
                                draw.matRefs.push_back(matRef);
                                if (verbose) fprintf(stderr, "[HSTF]     material[%zu]: pkg=%016llX ing=%016llX tgt=%08X\n",
                                    mi, (unsigned long long)matRef.pkg,
                                    (unsigned long long)matRef.ing, matRef.tgt);
                            }
                        }
                    }
                    else if (cls.find("MaterialPropertyOverrides") != std::string::npos) {
                        // Per-instance material overrides. The unlitatlasinstance coral carry
                        // "instance.atlasCellIndex" (value.x = which atlas cell = which colour variant). Without
                        // it every coral sampled cell 0 (one colour: coral_22 was blue, should be pink=cell 21).
                        if (dat.has("constantParameters") && dat["constantParameters"].isArray()) {
                            const auto& cps = dat["constantParameters"];
                            for (size_t ci = 0; ci < cps.size(); ++ci) {
                                const auto& cp = cps[ci].has("data") ? cps[ci]["data"] : tinyjson::Value();
                                if (!cp.has("parameterName") || !cp.has("value") || !cp["value"].has("x")) continue;
                                const std::string pn = cp["parameterName"].asString();
                                const float vx = (float)cp["value"]["x"].asFloat();
                                // VAT per-instance "instance" UBO {atlasCellIndex,animTrackIndex,animRateFactor,
                                // animTimeOffset} — each creature swims its OWN track/phase (else all snap to
                                // track 0 in sync → wrong pose → clip). Reversed from vatunlit* shader set2 bind1.
                                if      (pn.find("atlasCellIndex") != std::string::npos) draw.atlasCellIndex = vx;
                                else if (pn.find("animTrackIndex") != std::string::npos) draw.vatTrackIndex  = vx;
                                else if (pn.find("animRateFactor") != std::string::npos) draw.vatRateFactor  = vx;
                                else if (pn.find("animTimeOffset") != std::string::npos) draw.vatTimeOffset  = vx;
                                // Capture the FULL override (xyzw) for the renderer to apply over the cooked
                                // material block: matParams.tint (per-instance colour), matParams.windIntensityMax/
                                // ampMax/windSpeedMax/leafAmt/windDir (plant sway), RoughnessMultiplier/NormalGain,
                                // etc. Without these, grey butterflies stay grey + plants don't sway as authored.
                                MatOverride ov; ov.name = pn;
                                ov.v[0] = vx;
                                ov.v[1] = cp["value"].has("y") ? (float)cp["value"]["y"].asFloat() : 0.0f;
                                ov.v[2] = cp["value"].has("z") ? (float)cp["value"]["z"].asFloat() : 0.0f;
                                ov.v[3] = cp["value"].has("w") ? (float)cp["value"]["w"].asFloat() : 0.0f;
                                draw.matOverrides.push_back(ov);
                                if (verbose) fprintf(stderr, "[HSTF]     override %s = (%.3f,%.3f,%.3f,%.3f)\n",
                                    pn.c_str(), ov.v[0], ov.v[1], ov.v[2], ov.v[3]);
                            }
                        }
                    }
                    else if (cls == "horizon::platform_api::ScenePlatformComponent" && dat.isObject()) {
                        std::lock_guard<std::mutex> l(g_sceneFogMutex);
                        g_sceneFog.has = true;
                        if (dat.has("fogColor")) {
                            const auto& c = dat["fogColor"];
                            const char* k[4] = {"r", "g", "b", "a"};
                            for (int i = 0; i < 4; ++i) g_sceneFog.color[i] = c.has(k[i]) ? (float)c[k[i]].asFloat() : 0.0f;
                        }
                        if (!g_sceneFog.travel) {
                            if (dat.has("fogStart")) g_sceneFog.start = (float)dat["fogStart"].asFloat();
                            if (dat.has("fogDensity")) g_sceneFog.density = (float)dat["fogDensity"].asFloat();
                        }
                    }
                    else if (cls == "horizon::hpi::FogTravelComponent" && dat.isObject()) {
                        // Fog the home settles into after the travel-in: the last keyframe of each curve.
                        auto settled = [&](const char* curve, float& out) {
                            if (!dat.has(curve)) return;
                            const auto& cv = dat[curve];
                            if (!cv.has("data") || !cv["data"].has("keyframes")) return;
                            const auto& kf = cv["data"]["keyframes"];
                            if (!kf.isArray() || kf.size() == 0) return;
                            const auto& last = kf[kf.size() - 1];
                            if (last.has("data") && last["data"].has("value")) out = (float)last["data"]["value"].asFloat();
                        };
                        std::lock_guard<std::mutex> l(g_sceneFogMutex);
                        g_sceneFog.travel = true;
                        settled("propFogDistanceCurve", g_sceneFog.start);
                        settled("propFogDensityCurve", g_sceneFog.density);
                    }
                    else if (cls.find("SkyboxPlatformComponent") != std::string::npos) {
                        // The sky dome: its geometry is `skyboxMesh` and its texture is `colorTexture` — NOT a
                        // MeshPlatformComponent, so without this the skybox entity was dropped (hasMesh=false) =
                        // "No skybox support". Extract them so the dome renders with the panorama.
                        auto readRef = [&](const char* k) -> AssetKey {
                            if (!dat.has(k)) return {}; const auto& m = dat[k];
                            return { (u64)(m.has("packageOrRemoteId") ? std::stoull(m["packageOrRemoteId"].asString()) : 0),
                                     (u64)(m.has("ingestionId")      ? std::stoull(m["ingestionId"].asString()) : 0),
                                     (u32)(m.has("targetId")         ? (u32)m["targetId"].asInt() : 0) };
                        };
                        AssetKey sm = readRef("skyboxMesh");
                        if (sm.ing != 0) {
                            draw.meshRef = sm; draw.skyboxTex = readRef("colorTexture"); draw.isSkybox = true; hasMesh = true;
                            draw.skyboxRefl = readRef("reflectionMap");     // the scene's global IBL reflection cube
                            if (verbose) fprintf(stderr, "[HSTF]     SKYBOX mesh ing=%016llX colorTex ing=%016llX\n",
                                (unsigned long long)sm.ing, (unsigned long long)draw.skyboxTex.ing);
                        } else if (sceneRefl && !sceneRefl->ing) {
                            *sceneRefl = readRef("reflectionMap");
                        }
                    }
                }
            }
            if (hasMesh) out.push_back(draw);
        }
        if (verbose) fprintf(stderr, "[HSTF] Found %zu drawable entities\n", out.size());
        return !out.empty();
    } catch (const std::exception& e) {
        if (verbose) fprintf(stderr, "[HSTF] PARSE EXCEPTION: %s\n", e.what());
        return false;
    }
}
