#!/usr/bin/env python3
"""Generates src/clay_spirv.h: the built-in shaders Frame Home / Frame Studio draw Clay (.opa) homes with.

Clay homes (the pre-HSL Quest environments, e.g. cascadia.apk) carry no shader code: libshell builds its Clay
programs natively. Like Quest Home Editor's V79 path, they are drawn with a small unlit shader that outputs the
base texture's colour AND alpha (Clay textures carry their lighting baked in), times the vertex colour and the
per-draw tint, plus a cut-out variant for AlphaTest materials.

The interface uses the names Meta's own shaders use, so the renderer binds it with no special cases:
  vertex   in loc0 vec3 position, loc1 vec2 uv, loc2 vec4 color, loc3 vec3 normal, loc4 vec2 lightmapUv
           set0 b0 uniform GlobalUniforms { mat4 clipFromWorld[2]; }   (indexed by gl_ViewIndex: stereo-ready)
           push PushConstants { mat4 worldFromModel; vec4 color; vec4 uvTransformU; vec4 uvTransformV; }
           (vUV = (dot(uvTransformU.xyz, (uv, 1)), dot(uvTransformV.xyz, (uv, 1))): a Clay UV scroll / flipbook frame
            drawn without rewriting the vertices; the renderer pushes identity rows when a mesh has none)
           out loc0 vec2 vUV, loc1 vec4 vColor, loc2 vec3 vNormal, loc3 vec2 vUV1
  fragment set2 b0 sampler linearWrapSampler, set2 b1 texture2D baseColorTex, set2 b2 texture2D lightmapTex
           out = texture(baseColorTex, vUV) * vColor * vec4(texture(lightmapTex, vUV1).rgb, 1)
           vColor = the per-draw tint (BaseColorFactor x lightmappower): libshell's model shader (libshell.so @0x1e7550,
           the MeshShellEnv path) applies no vertex colour.
           (libshell's ShellEnv: diffuse x lightmap x lightmappower, the power riding in the tint; a mesh with no
            lightmap gets white. Cut-out: discard when alpha < 0.5.)
SPIR-V is hand-assembled (no shader compiler on this machine), every id and word count under our control.
Usage: python3 gen_clay_shader.py <output header>
"""
import struct
import sys

OpExtension = 10; OpCapability = 17; OpMemoryModel = 14; OpEntryPoint = 15; OpExecutionMode = 16
OpName = 5; OpMemberName = 6; OpDecorate = 71; OpMemberDecorate = 72
OpTypeVoid = 19; OpTypeBool = 20; OpTypeInt = 21; OpTypeFloat = 22; OpTypeVector = 23; OpTypeMatrix = 24
OpTypeImage = 25; OpTypeSampler = 26; OpTypeSampledImage = 27; OpTypeArray = 28; OpTypeStruct = 30
OpTypePointer = 32; OpTypeFunction = 33; OpConstant = 43; OpFunction = 54; OpFunctionEnd = 56; OpVariable = 59
OpLoad = 61; OpStore = 62; OpAccessChain = 65; OpVectorShuffle = 79; OpCompositeConstruct = 80; OpCompositeExtract = 81
OpSampledImage = 86; OpImageSampleImplicitLod = 87; OpFMul = 133; OpMatrixTimesVector = 145; OpDot = 148
OpFAdd = 129; OpFSub = 131; OpFDiv = 136; OpVectorTimesScalar = 142; OpSelect = 169
OpFOrdLessThan = 184; OpSelectionMerge = 247; OpLabel = 248; OpBranch = 249; OpBranchConditional = 250
OpKill = 252; OpReturn = 253

DEC_BLOCK = 2; DEC_COLMAJOR = 5; DEC_ARRAYSTRIDE = 6; DEC_MATSTRIDE = 7; DEC_BUILTIN = 11; DEC_LOCATION = 30
DEC_BINDING = 33; DEC_DESCSET = 34; DEC_OFFSET = 35
BUILTIN_POSITION = 0; BUILTIN_VIEWINDEX = 4440
SC_UNIFORMCONSTANT = 0; SC_INPUT = 1; SC_UNIFORM = 2; SC_OUTPUT = 3; SC_FUNCTION = 7; SC_PUSHCONSTANT = 9
CAP_SHADER = 1; CAP_MULTIVIEW = 4439


def strw(s):
    b = s.encode() + b"\0"
    while len(b) % 4:
        b += b"\0"
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


class Mod:
    def __init__(self):
        self.w = []
        self.next = 1

    def id(self):
        i = self.next
        self.next += 1
        return i

    def ins(self, op, *operands):
        flat = []
        for o in operands:
            flat.extend(o) if isinstance(o, (list, tuple)) else flat.append(o)
        self.w.append(((len(flat) + 1) << 16) | op)
        self.w.extend(flat)

    def words(self):
        return [0x07230203, 0x00010000, 0, self.next, 0] + self.w


def build_vert(skinned=False):
    """skinned: also in loc5 uvec4 boneIndices, loc6 vec4 boneWeights, set2 b3 uniform SkinningMatrices { mat4 joints[256]; }
    position = sum(w_i * joints[j_i] * position) / sum(w_i), or the position itself when its weights are all 0
    (the CPU skinning's rule)."""
    m = Mod()
    main, void, fnvoid, flt, i32, u32 = (m.id() for _ in range(6))
    v2, v3, v4, mat4, arr2, ubo_t, pc_t = (m.id() for _ in range(7))
    p_u_ubo, p_u_mat4, p_pc, p_pc_mat4, p_pc_v4 = (m.id() for _ in range(5))
    p_in_v3, p_in_v2, p_in_v4, p_in_i32, p_out_v2, p_out_v4, p_out_v3 = (m.id() for _ in range(7))
    c_one, c_i0, c_i1, c_u2 = (m.id() for _ in range(4))
    v_ubo, v_pc, v_pos, v_uv, v_col, v_nrm, v_view = (m.id() for _ in range(7))
    o_uv, o_col, o_nrm, o_glpos = (m.id() for _ in range(4))
    v_uv1, o_uv1, uv1 = (m.id() for _ in range(3))
    lbl, view, vp_ptr, vp, mdl_ptr, mdl, tint_ptr, tint, pos, px, py, pz, p4, wp, clip, uv, col, colt, nrm = (m.id() for _ in range(19))
    c_i2, c_i3, ru_ptr, ru, rv_ptr, rv, ru3, rv3, uu, uvv, uv3, tu, tv, tuv = (m.id() for _ in range(14))
    if skinned:
        uv4t, p_in_uv4, v_bidx, v_bwgt, c_u256, arr256, sk_t, p_u_sk, v_sk, boolt, c_eps, c_zero = (m.id() for _ in range(12))

    m.ins(OpCapability, CAP_SHADER)
    m.ins(OpCapability, CAP_MULTIVIEW)
    m.ins(OpExtension, strw("SPV_KHR_multiview"))
    m.ins(OpMemoryModel, 0, 1)
    m.ins(OpEntryPoint, 0, main, strw("main"), v_pos, v_uv, v_col, v_nrm, v_view, v_uv1, o_uv, o_col, o_nrm, o_uv1, o_glpos,
          *([v_bidx, v_bwgt] if skinned else []))
    if skinned:
        for i, n in [(v_bidx, "boneIndices"), (v_bwgt, "boneWeights"), (sk_t, "SkinningMatrices"), (v_sk, "sbSkinningMatrices")]:
            m.ins(OpName, i, strw(n))
        m.ins(OpMemberName, sk_t, 0, strw("joints"))
    for i, n in [(main, "main"), (ubo_t, "GlobalUniforms"), (v_ubo, "globalUniforms"), (pc_t, "PushConstants"),
                 (v_pc, "pushConstants"), (v_pos, "position"), (v_uv, "uv"), (v_col, "color"), (v_nrm, "normal"),
                 (o_uv, "vUV"), (o_col, "vColor"), (o_nrm, "vNormal"), (v_uv1, "lightmapUv"), (o_uv1, "vUV1")]:
        m.ins(OpName, i, strw(n))
    m.ins(OpMemberName, ubo_t, 0, strw("clipFromWorld"))
    m.ins(OpMemberName, pc_t, 0, strw("worldFromModel"))
    m.ins(OpMemberName, pc_t, 1, strw("color"))
    m.ins(OpMemberName, pc_t, 2, strw("uvTransformU"))
    m.ins(OpMemberName, pc_t, 3, strw("uvTransformV"))
    for var, loc in [(v_pos, 0), (v_uv, 1), (v_col, 2), (v_nrm, 3), (v_uv1, 4), (o_uv, 0), (o_col, 1), (o_nrm, 2), (o_uv1, 3)]:
        m.ins(OpDecorate, var, DEC_LOCATION, loc)
    m.ins(OpDecorate, o_glpos, DEC_BUILTIN, BUILTIN_POSITION)
    m.ins(OpDecorate, v_view, DEC_BUILTIN, BUILTIN_VIEWINDEX)
    m.ins(OpDecorate, arr2, DEC_ARRAYSTRIDE, 64)
    m.ins(OpMemberDecorate, ubo_t, 0, DEC_OFFSET, 0)
    m.ins(OpMemberDecorate, ubo_t, 0, DEC_COLMAJOR)
    m.ins(OpMemberDecorate, ubo_t, 0, DEC_MATSTRIDE, 16)
    m.ins(OpDecorate, ubo_t, DEC_BLOCK)
    m.ins(OpDecorate, v_ubo, DEC_DESCSET, 0)
    m.ins(OpDecorate, v_ubo, DEC_BINDING, 0)
    m.ins(OpMemberDecorate, pc_t, 0, DEC_OFFSET, 0)
    m.ins(OpMemberDecorate, pc_t, 0, DEC_COLMAJOR)
    m.ins(OpMemberDecorate, pc_t, 0, DEC_MATSTRIDE, 16)
    m.ins(OpMemberDecorate, pc_t, 1, DEC_OFFSET, 64)
    m.ins(OpMemberDecorate, pc_t, 2, DEC_OFFSET, 80)
    m.ins(OpMemberDecorate, pc_t, 3, DEC_OFFSET, 96)
    m.ins(OpDecorate, pc_t, DEC_BLOCK)
    if skinned:
        m.ins(OpDecorate, v_bidx, DEC_LOCATION, 5)
        m.ins(OpDecorate, v_bwgt, DEC_LOCATION, 6)
        m.ins(OpDecorate, arr256, DEC_ARRAYSTRIDE, 64)
        m.ins(OpMemberDecorate, sk_t, 0, DEC_OFFSET, 0)
        m.ins(OpMemberDecorate, sk_t, 0, DEC_COLMAJOR)
        m.ins(OpMemberDecorate, sk_t, 0, DEC_MATSTRIDE, 16)
        m.ins(OpDecorate, sk_t, DEC_BLOCK)
        m.ins(OpDecorate, v_sk, DEC_DESCSET, 2)
        m.ins(OpDecorate, v_sk, DEC_BINDING, 3)

    m.ins(OpTypeVoid, void)
    m.ins(OpTypeFunction, fnvoid, void)
    m.ins(OpTypeFloat, flt, 32)
    m.ins(OpTypeInt, i32, 32, 1)
    m.ins(OpTypeInt, u32, 32, 0)
    m.ins(OpTypeVector, v2, flt, 2)
    m.ins(OpTypeVector, v3, flt, 3)
    m.ins(OpTypeVector, v4, flt, 4)
    m.ins(OpTypeMatrix, mat4, v4, 4)
    m.ins(OpConstant, u32, c_u2, 2)
    m.ins(OpTypeArray, arr2, mat4, c_u2)
    m.ins(OpTypeStruct, ubo_t, arr2)
    m.ins(OpTypeStruct, pc_t, mat4, v4, v4, v4)
    if skinned:
        m.ins(OpTypeBool, boolt)
        m.ins(OpTypeVector, uv4t, u32, 4)
        m.ins(OpConstant, u32, c_u256, 256)
        m.ins(OpTypeArray, arr256, mat4, c_u256)
        m.ins(OpTypeStruct, sk_t, arr256)
        m.ins(OpTypePointer, p_u_sk, SC_UNIFORM, sk_t)
        m.ins(OpTypePointer, p_in_uv4, SC_INPUT, uv4t)
    m.ins(OpTypePointer, p_u_ubo, SC_UNIFORM, ubo_t)
    m.ins(OpTypePointer, p_u_mat4, SC_UNIFORM, mat4)
    m.ins(OpTypePointer, p_pc, SC_PUSHCONSTANT, pc_t)
    m.ins(OpTypePointer, p_pc_mat4, SC_PUSHCONSTANT, mat4)
    m.ins(OpTypePointer, p_pc_v4, SC_PUSHCONSTANT, v4)
    m.ins(OpTypePointer, p_in_v3, SC_INPUT, v3)
    m.ins(OpTypePointer, p_in_v2, SC_INPUT, v2)
    m.ins(OpTypePointer, p_in_v4, SC_INPUT, v4)
    m.ins(OpTypePointer, p_in_i32, SC_INPUT, i32)
    m.ins(OpTypePointer, p_out_v2, SC_OUTPUT, v2)
    m.ins(OpTypePointer, p_out_v4, SC_OUTPUT, v4)
    m.ins(OpTypePointer, p_out_v3, SC_OUTPUT, v3)
    m.ins(OpConstant, flt, c_one, f32(1.0))
    m.ins(OpConstant, i32, c_i0, 0)
    m.ins(OpConstant, i32, c_i1, 1)
    m.ins(OpConstant, i32, c_i2, 2)
    m.ins(OpConstant, i32, c_i3, 3)
    if skinned:
        m.ins(OpConstant, flt, c_eps, f32(1e-4))
        m.ins(OpConstant, flt, c_zero, f32(0.0))
        m.ins(OpVariable, p_u_sk, v_sk, SC_UNIFORM)
        m.ins(OpVariable, p_in_uv4, v_bidx, SC_INPUT)
        m.ins(OpVariable, p_in_v4, v_bwgt, SC_INPUT)
    m.ins(OpVariable, p_u_ubo, v_ubo, SC_UNIFORM)
    m.ins(OpVariable, p_pc, v_pc, SC_PUSHCONSTANT)
    m.ins(OpVariable, p_in_v3, v_pos, SC_INPUT)
    m.ins(OpVariable, p_in_v2, v_uv, SC_INPUT)
    m.ins(OpVariable, p_in_v4, v_col, SC_INPUT)
    m.ins(OpVariable, p_in_v3, v_nrm, SC_INPUT)
    m.ins(OpVariable, p_in_i32, v_view, SC_INPUT)
    m.ins(OpVariable, p_out_v2, o_uv, SC_OUTPUT)
    m.ins(OpVariable, p_out_v4, o_col, SC_OUTPUT)
    m.ins(OpVariable, p_out_v3, o_nrm, SC_OUTPUT)
    m.ins(OpVariable, p_out_v4, o_glpos, SC_OUTPUT)
    m.ins(OpVariable, p_in_v2, v_uv1, SC_INPUT)
    m.ins(OpVariable, p_out_v2, o_uv1, SC_OUTPUT)

    m.ins(OpFunction, void, main, 0, fnvoid)
    m.ins(OpLabel, lbl)
    m.ins(OpLoad, i32, view, v_view)
    m.ins(OpAccessChain, p_u_mat4, vp_ptr, v_ubo, c_i0, view)          # clipFromWorld[ViewIndex]
    m.ins(OpLoad, mat4, vp, vp_ptr)
    m.ins(OpAccessChain, p_pc_mat4, mdl_ptr, v_pc, c_i0)               # worldFromModel
    m.ins(OpLoad, mat4, mdl, mdl_ptr)
    m.ins(OpAccessChain, p_pc_v4, tint_ptr, v_pc, c_i1)                # color (per-draw tint)
    m.ins(OpLoad, v4, tint, tint_ptr)
    m.ins(OpLoad, v3, pos, v_pos)
    m.ins(OpCompositeExtract, flt, px, pos, 0)
    m.ins(OpCompositeExtract, flt, py, pos, 1)
    m.ins(OpCompositeExtract, flt, pz, pos, 2)
    m.ins(OpCompositeConstruct, v4, p4, px, py, pz, c_one)
    if skinned:
        bi, bw = m.id(), m.id()
        m.ins(OpLoad, uv4t, bi, v_bidx)
        m.ins(OpLoad, v4, bw, v_bwgt)
        acc = ws = None
        for k in range(4):
            j, w, mp, mj, vj, sj = (m.id() for _ in range(6))
            m.ins(OpCompositeExtract, u32, j, bi, k)
            m.ins(OpCompositeExtract, flt, w, bw, k)
            m.ins(OpAccessChain, p_u_mat4, mp, v_sk, c_i0, j)              # joints[j]
            m.ins(OpLoad, mat4, mj, mp)
            m.ins(OpMatrixTimesVector, v4, vj, mj, p4)
            m.ins(OpVectorTimesScalar, v4, sj, vj, w)
            if acc is None:
                acc, ws = sj, w
            else:
                a2, w2 = m.id(), m.id()
                m.ins(OpFAdd, v4, a2, acc, sj)
                m.ins(OpFAdd, flt, w2, ws, w)
                acc, ws = a2, w2
        none, f, g, den, inv, sk, a, b, pos_s = (m.id() for _ in range(9))
        m.ins(OpFOrdLessThan, boolt, none, ws, c_eps)                        # no weights: the bind position
        m.ins(OpSelect, flt, f, none, c_one, c_zero)
        m.ins(OpFSub, flt, g, c_one, f)
        m.ins(OpFAdd, flt, den, ws, f)
        m.ins(OpFDiv, flt, inv, c_one, den)
        m.ins(OpVectorTimesScalar, v4, sk, acc, inv)
        m.ins(OpVectorTimesScalar, v4, a, sk, g)
        m.ins(OpVectorTimesScalar, v4, b, p4, f)
        m.ins(OpFAdd, v4, pos_s, a, b)
        p4 = pos_s
    m.ins(OpMatrixTimesVector, v4, wp, mdl, p4)
    m.ins(OpMatrixTimesVector, v4, clip, vp, wp)
    m.ins(OpStore, o_glpos, clip)
    m.ins(OpLoad, v2, uv, v_uv)
    m.ins(OpAccessChain, p_pc_v4, ru_ptr, v_pc, c_i2)                 # uvTransformU / uvTransformV
    m.ins(OpLoad, v4, ru, ru_ptr)
    m.ins(OpAccessChain, p_pc_v4, rv_ptr, v_pc, c_i3)
    m.ins(OpLoad, v4, rv, rv_ptr)
    m.ins(OpVectorShuffle, v3, ru3, ru, ru, 0, 1, 2)
    m.ins(OpVectorShuffle, v3, rv3, rv, rv, 0, 1, 2)
    m.ins(OpCompositeExtract, flt, uu, uv, 0)
    m.ins(OpCompositeExtract, flt, uvv, uv, 1)
    m.ins(OpCompositeConstruct, v3, uv3, uu, uvv, c_one)
    m.ins(OpDot, flt, tu, ru3, uv3)
    m.ins(OpDot, flt, tv, rv3, uv3)
    m.ins(OpCompositeConstruct, v2, tuv, tu, tv)
    m.ins(OpStore, o_uv, tuv)
    # libshell's model shader has no vertex colour: colour = BaseColorFactor (here the tint) x textures.
    m.ins(OpStore, o_col, tint)
    m.ins(OpLoad, v3, nrm, v_nrm)
    m.ins(OpStore, o_nrm, nrm)
    m.ins(OpLoad, v2, uv1, v_uv1)
    m.ins(OpStore, o_uv1, uv1)
    m.ins(OpReturn)
    m.ins(OpFunctionEnd)
    return m.words()


def build_frag(cutout):
    m = Mod()
    main, void, fnvoid, flt, v2, v4, img_t, smp_t, simg_t, boolt = (m.id() for _ in range(10))
    p_uc_img, p_uc_smp, p_in_v2, p_in_v4, p_out_v4 = (m.id() for _ in range(5))
    v_tex, v_smp, i_uv, i_col, o_col, c_half = (m.id() for _ in range(6))
    lbl, img, smp, si, uv, texel, col, res = (m.id() for _ in range(8))
    a, cond, l_kill, l_go = (m.id() for _ in range(4))
    v_lm, i_uv1, lmimg, lmsi, uv1, lmtex, lr, lg, lb, c_one, lm4, base = (m.id() for _ in range(12))

    m.ins(OpCapability, CAP_SHADER)
    m.ins(OpMemoryModel, 0, 1)
    m.ins(OpEntryPoint, 4, main, strw("main"), i_uv, i_col, i_uv1, o_col)
    m.ins(OpExecutionMode, main, 7)                                    # OriginUpperLeft
    for i, n in [(main, "main"), (v_tex, "baseColorTex"), (v_smp, "linearWrapSampler"), (i_uv, "vUV"),
                 (i_col, "vColor"), (o_col, "outColor"), (v_lm, "lightmapTex"), (i_uv1, "vUV1")]:
        m.ins(OpName, i, strw(n))
    m.ins(OpDecorate, i_uv, DEC_LOCATION, 0)
    m.ins(OpDecorate, i_col, DEC_LOCATION, 1)
    m.ins(OpDecorate, o_col, DEC_LOCATION, 0)
    m.ins(OpDecorate, v_smp, DEC_DESCSET, 2)
    m.ins(OpDecorate, v_smp, DEC_BINDING, 0)
    m.ins(OpDecorate, v_tex, DEC_DESCSET, 2)
    m.ins(OpDecorate, v_tex, DEC_BINDING, 1)
    m.ins(OpDecorate, v_lm, DEC_DESCSET, 2)
    m.ins(OpDecorate, v_lm, DEC_BINDING, 2)
    m.ins(OpDecorate, i_uv1, DEC_LOCATION, 3)

    m.ins(OpTypeVoid, void)
    m.ins(OpTypeFunction, fnvoid, void)
    m.ins(OpTypeFloat, flt, 32)
    m.ins(OpTypeBool, boolt)
    m.ins(OpTypeVector, v2, flt, 2)
    m.ins(OpTypeVector, v4, flt, 4)
    m.ins(OpTypeImage, img_t, flt, 1, 0, 0, 0, 1, 0)                   # 2D, sampled, unknown format
    m.ins(OpTypeSampler, smp_t)
    m.ins(OpTypeSampledImage, simg_t, img_t)
    m.ins(OpTypePointer, p_uc_img, SC_UNIFORMCONSTANT, img_t)
    m.ins(OpTypePointer, p_uc_smp, SC_UNIFORMCONSTANT, smp_t)
    m.ins(OpTypePointer, p_in_v2, SC_INPUT, v2)
    m.ins(OpTypePointer, p_in_v4, SC_INPUT, v4)
    m.ins(OpTypePointer, p_out_v4, SC_OUTPUT, v4)
    m.ins(OpConstant, flt, c_half, f32(0.5))
    m.ins(OpConstant, flt, c_one, f32(1.0))
    m.ins(OpVariable, p_uc_img, v_tex, SC_UNIFORMCONSTANT)
    m.ins(OpVariable, p_uc_smp, v_smp, SC_UNIFORMCONSTANT)
    m.ins(OpVariable, p_in_v2, i_uv, SC_INPUT)
    m.ins(OpVariable, p_in_v4, i_col, SC_INPUT)
    m.ins(OpVariable, p_out_v4, o_col, SC_OUTPUT)
    m.ins(OpVariable, p_uc_img, v_lm, SC_UNIFORMCONSTANT)
    m.ins(OpVariable, p_in_v2, i_uv1, SC_INPUT)

    m.ins(OpFunction, void, main, 0, fnvoid)
    m.ins(OpLabel, lbl)
    m.ins(OpLoad, img_t, img, v_tex)
    m.ins(OpLoad, smp_t, smp, v_smp)
    m.ins(OpSampledImage, simg_t, si, img, smp)
    m.ins(OpLoad, v2, uv, i_uv)
    m.ins(OpImageSampleImplicitLod, v4, texel, si, uv)
    m.ins(OpLoad, v4, col, i_col)
    m.ins(OpFMul, v4, base, texel, col)
    m.ins(OpLoad, img_t, lmimg, v_lm)                                  # lightmap at uv1, rgb only
    m.ins(OpSampledImage, simg_t, lmsi, lmimg, smp)
    m.ins(OpLoad, v2, uv1, i_uv1)
    m.ins(OpImageSampleImplicitLod, v4, lmtex, lmsi, uv1)
    m.ins(OpCompositeExtract, flt, lr, lmtex, 0)
    m.ins(OpCompositeExtract, flt, lg, lmtex, 1)
    m.ins(OpCompositeExtract, flt, lb, lmtex, 2)
    m.ins(OpCompositeConstruct, v4, lm4, lr, lg, lb, c_one)
    m.ins(OpFMul, v4, res, base, lm4)
    if cutout:
        m.ins(OpCompositeExtract, flt, a, res, 3)
        m.ins(OpFOrdLessThan, boolt, cond, a, c_half)
        m.ins(OpSelectionMerge, l_go, 0)
        m.ins(OpBranchConditional, cond, l_kill, l_go)
        m.ins(OpLabel, l_kill)
        m.ins(OpKill)
        m.ins(OpLabel, l_go)
    m.ins(OpStore, o_col, res)
    m.ins(OpReturn)
    m.ins(OpFunctionEnd)
    return m.words()


def emit(name, words):
    lines = ["static const uint32_t %s[] = {" % name]
    for i in range(0, len(words), 8):
        lines.append("    " + ",".join("0x%08x" % w for w in words[i:i + 8]) + ",")
    lines.append("};")
    return "\n".join(lines)


def build_sel_vert():
    """Selection overlay: in loc0 vec3 position, loc1 vec2 uv; set0 b0 uniform { mat4 mvp }; out loc0 vec2 vUV."""
    m = Mod()
    main, void, fnvoid, flt, i32, v2, v3, v4, mat4, ubo_t = (m.id() for _ in range(10))
    p_u_ubo, p_u_mat4, p_in_v3, p_in_v2, p_out_v2, p_out_v4 = (m.id() for _ in range(6))
    c_one, c_i0, v_ubo, v_pos, v_uv, o_uv, o_glpos = (m.id() for _ in range(7))
    lbl, mptr, mvp, pos, px, py, pz, p4, clip, uv = (m.id() for _ in range(10))
    m.ins(OpCapability, CAP_SHADER)
    m.ins(OpMemoryModel, 0, 1)
    m.ins(OpEntryPoint, 0, main, strw("main"), v_pos, v_uv, o_uv, o_glpos)
    for i, n in [(main, "main"), (v_ubo, "selection"), (v_pos, "position"), (v_uv, "uv"), (o_uv, "vUV")]:
        m.ins(OpName, i, strw(n))
    m.ins(OpMemberName, ubo_t, 0, strw("mvp"))
    m.ins(OpDecorate, v_pos, DEC_LOCATION, 0)
    m.ins(OpDecorate, v_uv, DEC_LOCATION, 1)
    m.ins(OpDecorate, o_uv, DEC_LOCATION, 0)
    m.ins(OpDecorate, o_glpos, DEC_BUILTIN, BUILTIN_POSITION)
    m.ins(OpMemberDecorate, ubo_t, 0, DEC_OFFSET, 0)
    m.ins(OpMemberDecorate, ubo_t, 0, DEC_COLMAJOR)
    m.ins(OpMemberDecorate, ubo_t, 0, DEC_MATSTRIDE, 16)
    m.ins(OpDecorate, ubo_t, DEC_BLOCK)
    m.ins(OpDecorate, v_ubo, DEC_DESCSET, 0)
    m.ins(OpDecorate, v_ubo, DEC_BINDING, 0)
    m.ins(OpTypeVoid, void)
    m.ins(OpTypeFunction, fnvoid, void)
    m.ins(OpTypeFloat, flt, 32)
    m.ins(OpTypeInt, i32, 32, 1)
    m.ins(OpTypeVector, v2, flt, 2)
    m.ins(OpTypeVector, v3, flt, 3)
    m.ins(OpTypeVector, v4, flt, 4)
    m.ins(OpTypeMatrix, mat4, v4, 4)
    m.ins(OpTypeStruct, ubo_t, mat4)
    m.ins(OpTypePointer, p_u_ubo, SC_UNIFORM, ubo_t)
    m.ins(OpTypePointer, p_u_mat4, SC_UNIFORM, mat4)
    m.ins(OpTypePointer, p_in_v3, SC_INPUT, v3)
    m.ins(OpTypePointer, p_in_v2, SC_INPUT, v2)
    m.ins(OpTypePointer, p_out_v2, SC_OUTPUT, v2)
    m.ins(OpTypePointer, p_out_v4, SC_OUTPUT, v4)
    m.ins(OpConstant, flt, c_one, f32(1.0))
    m.ins(OpConstant, i32, c_i0, 0)
    m.ins(OpVariable, p_u_ubo, v_ubo, SC_UNIFORM)
    m.ins(OpVariable, p_in_v3, v_pos, SC_INPUT)
    m.ins(OpVariable, p_in_v2, v_uv, SC_INPUT)
    m.ins(OpVariable, p_out_v2, o_uv, SC_OUTPUT)
    m.ins(OpVariable, p_out_v4, o_glpos, SC_OUTPUT)
    m.ins(OpFunction, void, main, 0, fnvoid)
    m.ins(OpLabel, lbl)
    m.ins(OpAccessChain, p_u_mat4, mptr, v_ubo, c_i0)
    m.ins(OpLoad, mat4, mvp, mptr)
    m.ins(OpLoad, v3, pos, v_pos)
    m.ins(OpCompositeExtract, flt, px, pos, 0)
    m.ins(OpCompositeExtract, flt, py, pos, 1)
    m.ins(OpCompositeExtract, flt, pz, pos, 2)
    m.ins(OpCompositeConstruct, v4, p4, px, py, pz, c_one)
    m.ins(OpMatrixTimesVector, v4, clip, mvp, p4)
    m.ins(OpStore, o_glpos, clip)
    m.ins(OpLoad, v2, uv, v_uv)
    m.ins(OpStore, o_uv, uv)
    m.ins(OpReturn)
    m.ins(OpFunctionEnd)
    return m.words()


def build_sel_frag():
    """Selection overlay: set0 b1 combined sampler 'tex'; out = vec4(texture(tex, vUV).rgb, 1)."""
    m = Mod()
    main, void, fnvoid, flt, v2, v4, img_t, simg_t = (m.id() for _ in range(8))
    p_uc_simg, p_in_v2, p_out_v4, v_tex, i_uv, o_col, c_one = (m.id() for _ in range(7))
    lbl, si, uv, texel, r, g, b, res = (m.id() for _ in range(8))
    m.ins(OpCapability, CAP_SHADER)
    m.ins(OpMemoryModel, 0, 1)
    m.ins(OpEntryPoint, 4, main, strw("main"), i_uv, o_col)
    m.ins(OpExecutionMode, main, 7)
    for i, n in [(main, "main"), (v_tex, "tex"), (i_uv, "vUV"), (o_col, "outColor")]:
        m.ins(OpName, i, strw(n))
    m.ins(OpDecorate, i_uv, DEC_LOCATION, 0)
    m.ins(OpDecorate, o_col, DEC_LOCATION, 0)
    m.ins(OpDecorate, v_tex, DEC_DESCSET, 0)
    m.ins(OpDecorate, v_tex, DEC_BINDING, 1)
    m.ins(OpTypeVoid, void)
    m.ins(OpTypeFunction, fnvoid, void)
    m.ins(OpTypeFloat, flt, 32)
    m.ins(OpTypeVector, v2, flt, 2)
    m.ins(OpTypeVector, v4, flt, 4)
    m.ins(OpTypeImage, img_t, flt, 1, 0, 0, 0, 1, 0)
    m.ins(OpTypeSampledImage, simg_t, img_t)
    m.ins(OpTypePointer, p_uc_simg, SC_UNIFORMCONSTANT, simg_t)
    m.ins(OpTypePointer, p_in_v2, SC_INPUT, v2)
    m.ins(OpTypePointer, p_out_v4, SC_OUTPUT, v4)
    m.ins(OpConstant, flt, c_one, f32(1.0))
    m.ins(OpVariable, p_uc_simg, v_tex, SC_UNIFORMCONSTANT)
    m.ins(OpVariable, p_in_v2, i_uv, SC_INPUT)
    m.ins(OpVariable, p_out_v4, o_col, SC_OUTPUT)
    m.ins(OpFunction, void, main, 0, fnvoid)
    m.ins(OpLabel, lbl)
    m.ins(OpLoad, simg_t, si, v_tex)
    m.ins(OpLoad, v2, uv, i_uv)
    m.ins(OpImageSampleImplicitLod, v4, texel, si, uv)
    m.ins(OpCompositeExtract, flt, r, texel, 0)
    m.ins(OpCompositeExtract, flt, g, texel, 1)
    m.ins(OpCompositeExtract, flt, b, texel, 2)
    m.ins(OpCompositeConstruct, v4, res, r, g, b, c_one)
    m.ins(OpStore, o_col, res)
    m.ins(OpReturn)
    m.ins(OpFunctionEnd)
    return m.words()


def check(words):
    """Structural check: the instruction stream must end exactly at the last word (what broke QHE's fallback blobs)."""
    i = 5
    while i < len(words):
        wc = words[i] >> 16
        if wc == 0:
            raise SystemExit("zero word count at %d" % i)
        i += wc
    if i != len(words):
        raise SystemExit("instruction stream overruns the module")
    return words


if __name__ == "__main__" and len(sys.argv) > 2 and sys.argv[1] == "--selection":
    out = sys.argv[2]
    body = ["#pragma once",
            "// GENERATED by tools/gen_clay_shader.py --selection: the selection-highlight overlay shaders. Do not edit by hand.",
            "//   vert: in vec3 position @0, vec2 uv @1; set0 b0 uniform { mat4 mvp }   frag: set0 b1 sampler2D tex -> rgb, a=1",
            "#include <cstddef>", "#include <cstdint>", "",
            emit("kSelectionVertSpirv", check(build_sel_vert())),
            "static const size_t kSelectionVertSize = sizeof(kSelectionVertSpirv);", "",
            emit("kSelectionFragSpirv", check(build_sel_frag())),
            "static const size_t kSelectionFragSize = sizeof(kSelectionFragSpirv);", ""]
    open(out, "w", newline="\n").write("\n".join(body))
    print("wrote", out)
elif __name__ == "__main__":
    out = sys.argv[1]
    body = ["#pragma once",
            "// GENERATED by tools/gen_clay_shader.py: the built-in Clay (.opa home) shaders. Do not edit by hand.",
            "#include <cstdint>", "",
            emit("kClayVertSpirv", build_vert()), "",
            emit("kClayFragSpirv", build_frag(False)), "",
            emit("kClayFragCutoutSpirv", build_frag(True)), "",
            emit("kClaySkinnedVertSpirv", check(build_vert(True))), ""]
    open(out, "w", newline="\n").write("\n".join(body))
    print("wrote", out)
