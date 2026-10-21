#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <array>
#include <memory>
#include <mutex>
#include <tuple>

using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i32 = int32_t;

// A T read from file bytes at any address: the cooked formats pack fields on 1- and 2-byte boundaries, and reading
// them through a cast pointer is undefined behaviour a compiler may exploit. memcpy compiles to the same one load.
template <typename T> inline T loadUnaligned(const void* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

// MurmurHash3 x86 32-bit (canonical, seed-parameterised). libshell hashes cooked material constant
// names with this (seed 0) over the fully-qualified uniform name "matParams.<member>" — confirmed by
// reversing setUniformByHash call sites (compile-time hash + literal name) and reproducing every env
// material's constantParameter nameHashes. This is the KEY that binds cooked constants to shader UBO slots.
inline u32 murmur3_x86_32(const void* key, size_t len, u32 seed = 0) {
    const u8* data = static_cast<const u8*>(key);
    const size_t nblocks = len / 4;
    u32 h1 = seed;
    const u32 c1 = 0xcc9e2d51u, c2 = 0x1b873593u;
    auto rotl = [](u32 x, int r) -> u32 { return (x << r) | (x >> (32 - r)); };
    for (size_t i = 0; i < nblocks; ++i) {
        u32 k1;
        std::memcpy(&k1, data + i * 4, 4);          // little-endian host (Windows x64)
        k1 *= c1; k1 = rotl(k1, 15); k1 *= c2;
        h1 ^= k1; h1 = rotl(h1, 13); h1 = h1 * 5 + 0xe6546b64u;
    }
    const u8* tail = data + nblocks * 4;
    u32 k1 = 0;
    switch (len & 3) {
        case 3: k1 ^= (u32)tail[2] << 16; [[fallthrough]];
        case 2: k1 ^= (u32)tail[1] << 8;  [[fallthrough]];
        case 1: k1 ^= (u32)tail[0];
                k1 *= c1; k1 = rotl(k1, 15); k1 *= c2; h1 ^= k1;
    }
    h1 ^= (u32)len;
    h1 ^= h1 >> 16; h1 *= 0x85ebca6bu; h1 ^= h1 >> 13; h1 *= 0xc2b2ae35u; h1 ^= h1 >> 16;
    return h1;
}

// Hash of "matParams.<member>" — the cooked constantParameter nameHash for a shader UBO member.
inline u32 matParamNameHash(const std::string& memberName) {
    std::string full = "matParams." + memberName;
    return murmur3_x86_32(full.data(), full.size(), 0);
}

struct AssetKey {
    u64 pkg = 0;
    u64 ing = 0;
    u32 tgt = 0;

    bool operator==(const AssetKey& o) const {
        return pkg == o.pkg && ing == o.ing && tgt == o.tgt;
    }
};

struct AssetKeyHash {
    size_t operator()(const AssetKey& k) const {
        auto h1 = std::hash<u64>{}(k.pkg);
        auto h2 = std::hash<u64>{}(k.ing);
        auto h3 = std::hash<u32>{}(k.tgt);
        return h1 ^ (h2 << 1) ^ (h3 << 2);
    }
};

using AssetMap = std::unordered_map<AssetKey, std::string, AssetKeyHash>;

// One cooked material constant (MATLMATL `constantParameters` entry =
// horizon::renderer::ConstantMaterialParameter). The shader UBO member it targets is found
// by MurmurHash3_x86_32("matParams."+memberName, 0) == nameHash (reversed from libshell
// setUniformByHash). value = matParamsBlob[blobOffset .. +byteSize], tight-packed (scalar 4B, vec3 12B).
struct ConstParam {
    u32 nameHash = 0;
    u32 blobOffset = 0;
    u32 byteSize = 0;
};

struct Transform {
    float pos[3] = {0, 0, 0};
    float rot[4] = {0, 0, 0, 1}; // xyzw quaternion
    float scale[3] = {1, 1, 1};
};

// Column-major T·R·S local matrix (scale in local space, THEN rotate, THEN translate — libshell's per-node
// local matrix). Rotation columns are scaled => M = T·R·S, never T·S·R.
inline void trsToMat4(const Transform& t, float out[16]) {
    float x=t.rot[0], y=t.rot[1], z=t.rot[2], w=t.rot[3];
    float sx=t.scale[0], sy=t.scale[1], sz=t.scale[2];
    float r00=1-2*(y*y+z*z), r01=2*(x*y-w*z), r02=2*(x*z+w*y);
    float r10=2*(x*y+w*z), r11=1-2*(x*x+z*z), r12=2*(y*z-w*x);
    float r20=2*(x*z-w*y), r21=2*(y*z+w*x), r22=1-2*(x*x+y*y);
    out[0]=r00*sx; out[4]=r01*sy; out[8]=r02*sz; out[12]=t.pos[0];
    out[1]=r10*sx; out[5]=r11*sy; out[9]=r12*sz; out[13]=t.pos[1];
    out[2]=r20*sx; out[6]=r21*sy; out[10]=r22*sz; out[14]=t.pos[2];
    out[3]=0;      out[7]=0;      out[11]=0;      out[15]=1;
}
// out = a · b (column-major). Composing parent·child as 4×4 matrices preserves the SHEAR a non-uniform-scale
// parent × rotated child produces — which a TRS-only recompose silently drops (the "stretching/slanting").
inline void mat4mul4(const float a[16], const float b[16], float out[16]) {
    for (int col=0; col<4; ++col)
        for (int row=0; row<4; ++row) {
            float s=0; for (int k=0;k<4;++k) s += a[k*4+row]*b[col*4+k];
            out[col*4+row]=s;
        }
}

// Generic captured component (every hstf component, even ones the typed pipeline ignores) so the EDITOR can
// inspect/edit ALL of them + any extras. `fields` is the component's data flattened to name->display string
// (nested objects flattened with dotted keys, e.g. "lightmapUvScale.x"). Layout truth: V203_COMPONENTS_IDA.md.
struct EnvComponent {
    std::string cls;                 // full class, e.g. "horizon::platform_api::ScenePlatformComponent"
    std::string shortCls;            // trailing name, e.g. "ScenePlatformComponent"
    int         version = 0;
    std::vector<std::pair<std::string,std::string>> fields;   // (dotted key, value) in declaration order
};

// Global exposure applied to baked HDR lightmaps before the ACES tonemap (rendtxtr_parser). 2.6 is tuned for
// DARK INTERIOR envs (horror/candle/chair) that came out too dim. Bright OUTDOOR VISTAS get over-exposed at 2.6
// (the lightmapped ground/boulders clip toward white), so main.cpp lowers this for vista_* envs. HSR_LMEXP overrides.
inline float g_lmExposure = 2.6f;

// A single MaterialPropertyOverrides "constantParameter": the per-instance override of a shader matParams
// member (e.g. "matParams.tint", "matParams.windIntensityMax") or an "instance.*" value. The device applies
// these OVER the cooked material's constant block, so a grey butterfly gets its colour, plants get their wind,
// props get their per-instance roughness/atlas cell. value is xyzw (most are scalar in .x).
struct MatOverride { std::string name; float v[4] = {0.0f,0.0f,0.0f,0.0f}; };

struct DrawableEntity {
    std::string name;
    std::string path;                       // the groups (parent entities, template instances) it sits in, '/'-separated
    Transform   transform;
    std::vector<EnvComponent> components;   // ALL components on this entity (generic inspect/edit)
    AssetKey    meshRef;
    std::vector<AssetKey> matRefs;
    float worldMatrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1}; // faithful 4×4 parent→child world matrix
    bool  hasWorldMatrix = false;
    bool  emptyTransform = false;   // had a TransformPlatformComponent but NO local pos/rot/scale (cook anomaly — resolve via level group offset)
    float atlasCellIndex = -1.0f;   // MaterialPropertyOverrides "instance.atlasCellIndex" (per-instance atlas variant; -1 = none)
    // VAT per-instance anim (the `instance` UBO {atlasCellIndex,animTrackIndex,animRateFactor,animTimeOffset});
    // each creature swims its own track/phase. -1/0 defaults if the entity has no override.
    float vatTrackIndex = -1.0f, vatRateFactor = -1.0f, vatTimeOffset = -1.0f;
    // ALL MaterialPropertyOverrides "matParams.*"/"instance.*" overrides for this entity (per-instance tint,
    // wind/sway, roughness, atlas cell, anim variation). Applied OVER the cooked material block by the renderer.
    std::vector<MatOverride> matOverrides;
    // SkyboxPlatformComponent: skyboxMesh -> meshRef, colorTexture -> skyboxTex (a direct TextureAsset, NOT via a
    // material). isSkybox routes the loader to bind skyboxTex as the base texture + mark the mesh as the sky dome.
    AssetKey skyboxTex{}; bool isSkybox = false;
    AssetKey skyboxRefl{};      // SkyboxPlatformComponent.reflectionMap -> globalIblReflectionCubeMap
};

// A texture kept compressed until the renderer needs it: uploaded as ASTC where the GPU samples ASTC, or
// decoded in the background and streamed in (the scene appears first). Set g_deferTextures to load this way.
struct TexSource {
    std::vector<u8> astc;           // the mip chain as cooked: mip0 first, `mips` levels
    u32 w = 0, h = 0, bw = 4, bh = 4, mips = 1;
    bool hdr = false;
    u32 faces = 1;                  // 6: a cubemap, mip-major (six faces of each level)
};
inline bool g_deferTextures = false;

// The world's fog (ScenePlatformComponent; a FogTravelComponent's settled curve values win), as HSR Studio
// passes it to the frame globals verbatim. Zero = no fog. Written by the HSTF parser (any thread).
struct SceneFog { bool has = false; float color[4] = {0, 0, 0, 0}; float start = 0, density = 0; bool travel = false; };
inline SceneFog g_sceneFog;
inline std::mutex g_sceneFogMutex;

struct MeshData {
    std::string name;
    std::string path;                       // where it sits in its file: the groups / nodes / templates holding it, '/'-separated
    std::vector<EnvComponent> components;   // all hstf components of the owning entity (generic editor inspect/edit)
    bool isSkybox = false;                  // SkyboxPlatformComponent dome (colorTexture panorama; opaque, no far-clip)
    std::vector<float> positions;  // xyz * nVerts
    std::vector<float> uvs;        // uv * nVerts
    std::vector<u8>    colors;     // sem4 per-vertex COLOR (u8x4 RGBA) = device vertexColor0 (animvege leaf bend-mask / butterfly wing colour); empty if the mesh has no colour attribute
    std::vector<float> uvs3, uvs4; // sem5 idx2/idx3 (uv2/uv3): animvege packed per-vertex flutter phase/pivot
    std::vector<u32>    indices;   // triangle list (32-bit: large glTF meshes exceed 65535 verts)
    u32 nVerts = 0;
    u32 nIdx   = 0;

    // Base-color texture (decoded RGBA). For isotropictiled this is BaseColorMetallic_Tx
    // (RGB=base color, A=metallic).
    std::vector<u8> texRGBA;
    u32 texW = 1, texH = 1;
    bool hasTexture = false;
    // SPLIT-PART TEXTURE SHARING: >=0 = this mesh is a piece from splitMeshParts that BORROWS all its texture/
    // lightmap byte buffers from sceneMeshes[texShareSrc] (the source it was carved from) instead of keeping its
    // own copy — the renderer (VkRenderer::texShareList) and the cook resolve it. Prevents the up-to-256-pieces
    // × 16MB-texture memory blow-up that crashed splitting a merged statue. -1 = normal self-owned textures.
    int texShareSrc = -1;
    // Source ASTC mip chain (blocks only) + footprint for LOSSLESS cook pass-through of an unmodified base texture
    // (avoids the decode→re-encode double-compression that softens definition). Empty if not a plain KTX-ASTC.
    std::vector<u8> srcAstc; u32 srcAstcBw = 0, srcAstcBh = 0, srcAstcMips = 0;

    // Additional per-material textures, bound to shader slots BY ROLE (libshell binds
    // each shader texture resource by name to the material's named texture param):
    //   normal  -> ONxRNy_Tx  (packed occlusion/normal/roughness)
    //   orm     -> ORM/roughness/metallic/AO slot (if the shader has one)
    //   lightmap-> baked lightmap slot
    std::vector<u8> normalRGBA;  u32 normalW = 1, normalH = 1; bool hasNormal = false;
    std::vector<u8> ormRGBA;     u32 ormW = 1, ormH = 1;       bool hasOrm = false;
    std::string ormTexName;      // ORM/rbaodir texture path — carries the lightmap MERGE-GROUP (e.g. "merged_ceilingtrim"): the rbaodir + the lmhdr lightmap for a baked mesh-group share this stem, so a mesh whose own name misses (misspelled "cieling") can still find its lightmap via the ORM's group.
    std::vector<u8> emissiveRGBA; u32 emissiveW = 1, emissiveH = 1; bool hasEmissive = false;
    std::vector<u8> lmRGBA;      u32 lmW = 1, lmH = 1;         bool hasLightmap = false;
    // The baked lightmap as linear HDR radiance (RGBA16F) — what the device samples; lmRGBA is a tonemapped
    // preview. lmUvScaleOffset = the entity's lightingParams {lightmapUvScale.xy, lightmapUvOffset.xy}: where
    // this mesh's UV2 patch sits inside a shared lightmap atlas (shader lightBakerParams {uvScale, uvOffset}).
    std::shared_ptr<const std::vector<u16>> lmHalf;     // shared: one atlas serves many meshes
    // Deferred mode: the lightmap still as its cooked ASTC (HDR profile) chain instead of lmHalf. GPUs with
    // textureCompressionASTC_HDR (the Frame) sample it directly; others decode it to half floats at upload.
    std::shared_ptr<const TexSource> lmSrc;
    std::shared_ptr<const TexSource> lmLdrSrc;   // Clay: the 8-bit (sRGB) lightmap still compressed, shared; lmRGBA empty
    bool hasLmHdr() const { return lmHalf || lmSrc; }
    // Cubemap base texture (the skybox colorTexture): six RGBA8 faces of cubeSize^2, +X -X +Y -Y +Z -Z.
    std::vector<u8> cubeRGBA; u32 cubeSize = 0;
    std::shared_ptr<const TexSource> cubeSrc;   // deferred: the same cube still as ASTC (cubeRGBA empty)
    // Deferred (still compressed) versions of the base / normal / ORM / emissive maps; when set, the matching
    // *RGBA vector is empty and has*/W/H describe it.
    std::shared_ptr<const TexSource> texSrc, normalSrc, ormSrc, emissiveSrc;
    // Every material texture keyed by its shader sampler's MurmurHash3 name (deferred mode).
    std::vector<std::pair<u32, std::shared_ptr<const TexSource>>> namedTex;
    // HDR textures a material binds by sampler name (reflection panoramas such as Cascadia's reflectionPano):
    // linear RGBA half floats, decoded once and shared.
    struct HdrTex { std::shared_ptr<const std::vector<u16>> half; u32 w = 0, h = 0; std::shared_ptr<const TexSource> src; };   // src: deferred ASTC
    std::vector<std::pair<u32, HdrTex>> namedHdr;
    // The skybox's reflectionMap: the scene's global IBL reflection cube, six RGBA16F (linear HDR) faces.
    // The IBL reflection cube, every cooked mip (each level pre-blurred for a roughness): mip-major, six faces per
    // level. Rough surfaces sample the blurred levels; with mip 0 alone every surface reflected like a mirror.
    std::vector<u16> reflCubeHalf; u32 reflCubeSize = 0, reflCubeMips = 1;
    std::shared_ptr<const TexSource> reflCubeSrc;   // the same, still ASTC HDR as cooked (decoded at upload); wins over reflCubeHalf
    float lmUvScaleOffset[4] = {1, 1, 0, 0};
    // VAT (Vertex Animation Texture) offset texture: R16G16B16A16_SFLOAT, width=vertexCount, height=frameCount.
    // The vatunlit vertex shader does worldPosition = inPos + texelFetch(vatAnimTex, (vertexCol, frameRow)).
    // vatRaw holds mip0 as raw half-float bytes (vatW*vatH*8), uploaded verbatim as R16G16B16A16_SFLOAT.
    std::vector<u8> vatRaw;      u32 vatW = 0, vatH = 0;       bool hasVat = false;
    // matParams.Tint (per-material RGB color multiplier; default white).
    float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    // Per-instance atlas variant (unlitatlasinstance shader's instanceTag.atlasCellIndex) from the entity's
    // MaterialPropertyOverrides "instance.atlasCellIndex". -1 = none (all coral were sampling cell 0 -> one colour).
    float atlasCellIndex = -1.0f;
    // VAT per-instance anim (vatunlit* `instance` UBO set2 bind1): animTrackIndex/animRateFactor/animTimeOffset.
    // Without these every creature snaps to track 0 in sync (wrong pose → turtle clips the deck). -1 = unset.
    float vatTrackIndex = -1.0f, vatRateFactor = -1.0f, vatTimeOffset = -1.0f;
    // Per-instance MaterialPropertyOverrides for this mesh's entity: "matParams.*" override the cooked material
    // constant block (tint/wind/roughness/normalGain), applied by name-hash in the renderer's matParams fill.
    std::vector<MatOverride> matOverrides;
    // Ingestion id of the RENDSHAD this material references — the renderer picks the
    // matching shader pipeline per mesh (masked->discard, emissive->glow, etc.), as libshell does.
    u64 shaderIng = 0;
    // Resolved shader asset path (".../NAME.surface/shader...") — used to apply matParamsBlob only
    // when this material's shader matches the renderer's chosen global shader (same UBO layout).
    std::string shaderPath;
    // Resolved mesh asset path (".../hero_bat_01.fbx/__mesh.../rootnode...") — used to match a skinned mesh
    // to ITS skeleton (shared .fbx path) for the multi-rig AnimGroup binding.
    std::string meshPath;
    // Material name carries "pbrlightmap_tiled" -> a genuinely TILED prop material (rugs etc.): unlike the
    // unwrap props (bowls/vases), it NEEDS its cooked matParams (Tint + GlobalTile) kept, like the room.
    bool tiled = false;
    // Cooked per-material matParams VALUE blob (real Tint/LayerRed/LayerBlue/Metallic/... values),
    // tight-packed (scalar 4B, vec3 12B) in the material's .surface param-declaration order — NOT the
    // SPIR-V std140 UBO order. Empty if the material has no constants.
    std::vector<u8> matParamsBlob;
    // The cooked `constantParameters` entries {nameHash, blobOffset, byteSize}. Each slices matParamsBlob
    // and binds BY NAME (MurmurHash3 of "matParams."+member) to the shader UBO offset — the faithful path
    // (the blob order ≠ UBO order, so positional binding swaps e.g. Tint/AOfloor → wrong colours).
    std::vector<ConstParam> constParams;
    // Candidate sampler-slot name-hashes (MurmurHash3 of the sampler name, e.g. "BaseColor_Tx"/"RBAoDir_Tx")
    // from the material's textureParameters — used to pick the shader VARIANT whose samplers match.
    std::vector<u32> texSlots;

    // Raw ASTC data for GPU-native upload (preferred when GPU supports ASTC_LDR)
    std::vector<u8> astcRaw;
    u32 astcBw = 0, astcBh = 0;

    // Transform
    Transform transform;
    // Faithful 4×4 world matrix (parent→child composed via mat4mul4). When set, the renderer uses this
    // verbatim instead of rebuilding T·R·S from `transform` — so hierarchy shear (non-uniform parent scale
    // × rotated child) is preserved, not dropped by a TRS recompose.
    float worldMatrix[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    bool  hasWorldMatrix = false;

    // Bone skinning data (for unlitblendskinned meshes, stride=28)
    std::vector<u8> boneIndices;  // 4 u8 per vertex (bone indices = SLOTS into bonePalette)
    std::vector<u8> boneWeights;  // 4 u8 per vertex (bone weights UNORM)
    bool hasBones = false;
    // Bone PALETTE = the mesh's explicit slot->joint MAP (RENDMESH ROOT.f2[0].f0): one
    // MurmurHash3_x86_32(jointName,0) per palette slot. Vertex boneIndices are SLOT indices; each slot
    // maps to the skeleton joint whose name-hash matches. This is what libshell reads (NOT a DFS guess).
    std::vector<u32> bonePalette;

    // Blend mode: true = SRC_ALPHA blend (dome/motes); false = opaque (floor/fallback)
    bool useBlend = false;

    // Additive blend (libshell material Additive:true — god-rays / light shafts / glow). Goes to
    // the blend pass but with ADD blending (dst += src) so black texels add nothing and the bright
    // shaft adds light. Without it, an additive light-shaft renders as an opaque dark rectangle.
    bool additive = false;

    // Alpha-test cutout: opaque pass, depth-write ON, fragment discards below threshold
    // (libshell AlphaTest materials — flags/foliage/animals). Distinct from useBlend.
    bool alphaTest = false;
    float alphaCutoff = 0.5f;   // AUTHORED 'alphatestthreshold' uniform (zen tree canopy = 0.25) — the V79
                                // frag discards color.a < AlphaCutoff; hardcoding 0.5 ate its mid-alpha leaves
    // TEX-AUDIT provenance: the RAW authored transparency (BEFORE any loader heuristic) so the cook audit can
    // diff "what the author wrote" against "what ships". srcAlphaMode: glTF alphaMode string only —
    // 0=OPAQUE 1=MASK 2=BLEND, -1=no material. srcFactorA = baseColorFactor alpha (1 when absent).
    int   srcAlphaMode = -1;
    float srcFactorA   = 1.f;

    // Editor overlay kind: normally these meshes are authored alongside render geometry but NOT
    // drawn (collision/navigation). The editor loads them as translucent flat-colour overlays so
    // navmeshes/collision can be inspected + edited. 0=normal visible mesh, 1=navmesh (green),
    // 2=collision/wall (red), 3=phys (orange).
    int overlayKind = 0;

    // glTF face culling: a material is doubleSided=false by default (single-sided -> back-face
    // cull, matching libshell). doubleSided=true -> no cull. Cel-shading inverted-hull OUTLINE
    // meshes are single-sided: without back-face culling the fattened black hull renders as a
    // solid blob that covers the whole interior ("fully dark"); culled, it shows only edge rims.
    // (OPA meshes keep the default true => CULL_NONE, unchanged.)
    bool doubleSided = true;

    // Dynamic vertices: positions are rewritten every frame (glTF skeletal animation).
    // The renderer keeps this mesh's VBO persistently mapped so an external animator can
    // stream skinned positions in. gltfMeshIndex links it back to the GltfLoader record.
    bool dynamicVerts = false;
    int  gltfMeshIndex = -1;
    // Runtime animation (the loaders' animateRuntime): a rigid node motion is drawn as a matrix on top of the mesh's
    // world (animXform = node(t) * node(0)^-1, positions stay at the t=0 pose), and posGen / uvGen count the rewrites
    // that changed positions / uvs, so the renderer re-uploads only those (and only the half that changed).
    float animXform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    float uvXform[6] = {1,0,0, 0,1,0};   // a UV scroll / flipbook frame over the uvs: u' = [0]u+[1]v+[2], v' = [3]u+[4]v+[5]
    uint32_t posGen = 0, uvGen = 0;
    // GPU skinning (Clay, animateRuntime): the vertex buffer holds skinBindPos (the bind pose; positions stays the world
    // rest pose) with boneIndices / boneWeights; each frame the loader writes the joint matrices (skinJoints, column-major,
    // <= 256) and the box the skinned mesh covers (skinBox: min xyz, max xyz), and bumps skinGen.
    std::vector<float> skinBindPos, skinJoints;
    float skinBox[6] = {0,0,0, 0,0,0};
    uint32_t skinGen = 0;

    // IBL-lit (V79 SpecIbl + no-albedo shells): the renderer bakes diffuseCube(worldN)·ambientTint
    // into the per-vertex color so these env-lit surfaces show the environment light instead of the
    // white/dark no-texture fallback. Set for *_specibl materials AND any opaque mesh whose albedo
    // texture didn't resolve (phantom Id) — both are lit purely by IBL in these homes.
    bool iblLit = false;

    // Full split-sum IBL (V79 SpecIbl no-albedo metallic/gem shells, e.g. divingHelmet/rubyGem):
    // the per-vertex colour is the complete specular-IBL reduction, not just diffuse irradiance —
    //   diffuse  = (1-metallic)·albedo·diffuseCube(N)·specibldiffusescale
    //   specular = F0·mix(specularCube(N), diffuseCube(N), roughness)·speciblspecularscale   (F0 = mix(0.04,albedo,metallic))
    //   colour   = (diffuse + specular)·ambientIBLTint
    // Read VERBATIM from the material's cooked uniforms (metallic/roughness/basecolor + the 4 specibl
    // params). Scoped to no-albedo-texture shells so textured *_specibl (goldmine ground) is untouched.
    bool  iblFullSpec = false;
    // Baked-lightmap lighting for TEXTURED interior shells (ShellEnv concrete/floor/walls). libshell
    // shades these `colour = diffuse(uv0·repeatuv) · lightmap(uv1) · lightmappower`. The lightmappower
    // is a per-channel HDR boost that produces the coloured neon/glow (concrete≈[3.8,3.2,4.4] pink,
    // floor≈[17.6,8.3,3.2] warm). We sample the lightmap (md.lmRGBA) at uv1 per vertex, scale by
    // lightmapPower, and bake into the per-vertex colour the fragment already multiplies by.
    bool  bakeLightmapVtx = false;
    std::vector<float> uvs2;        // uv1 = the lightmap unwrap (StdData a_texcoords.zw @16)
    bool alphaToCoverage = false;   // SHAD forward pass: cut-out foliage (alpha-to-coverage, opaque queue)
    std::vector<float> normals;     // authored NORMAL (VS sem 1), xyz per vertex; zero where a part has none
    std::vector<float> tangents;    // authored TANGENT (VS sem 2), xyzw per vertex (w = handedness); zero if none
    float lightmapPower[3] = {1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;          // 'metallic' uniform (0 dielectric .. 1 metal)
    float roughness = 1.0f;         // 'roughness' uniform (clamped 0..1 for the mip mix)
    float speciblDiffScale = 1.0f;  // 'specibldiffusescale'
    float speciblSpecScale = 1.0f;  // 'speciblspecularscale'
    float albedoFactor[3] = {1.0f, 1.0f, 1.0f};  // 'basecolor' factor (linear) = the metal/gem tint

    // Per-frame material tint (rgba) = the mat.sanim MaterialTint track sampled at the current
    // time. Default white (1,1,1,1 = no effect). For fog/dust/flicker materials this carries the
    // animated OPACITY (alpha 0..~0.22) that keeps the fog faint and pulsing. animate(t) updates
    // it; the renderer pushes it as the fragment shader's UniformColor (frag = texture * tint).
    float curTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    // Set when this mesh has an animated MaterialTint ALPHA fade (fog/dust: alpha sweeps 0→peak→0).
    // A fading alpha is definitionally an ALPHA-BLEND (the fade only shows through SRC_ALPHA blending),
    // so the renderer must NOT auto-promote it to the hard-additive (src=ONE) pipeline — that ignores
    // alpha and freezes the dust at full brightness (the "fade not decoded" bug). See vk_renderer premultAdd.
    bool animatedTintAlpha = false;
};

// RENDTXTR format codes → ASTC block sizes
inline void astcBlockSize(u8 fmtCode, u32& bw, u32& bh) {
    switch (fmtCode) {
        // Current Meta RENDTXTR enum pairs: linear / sRGB.
        case 15: case 16: bw = 4;  bh = 4;  break;
        case 17: case 18: bw = 6;  bh = 6;  break;
        case 19: case 20: bw = 8;  bh = 8;  break;
        case 21: case 22: bw = 10; bh = 10; break;
        case 23: case 24: bw = 12; bh = 12; break;
        // Older aliases retained for source compatibility.
        case  8: bw = 8;  bh = 8;  break;
        case 10: bw = 4;  bh = 4;  break;
        case 11: bw = 8;  bh = 8;  break;
        case 12: bw = 6;  bh = 6;  break;
        case 13: bw = 12; bh = 12; break;
        case 14: bw = 4;  bh = 4;  break;
        default: bw = 8;  bh = 8;  break;
    }
}
