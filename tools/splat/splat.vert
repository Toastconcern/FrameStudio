#version 450
// Gaussian splats (src/splat_import.h), one instanced quad each, both eyes in one multiview draw.
// Each splat: a 3D Gaussian (centre, covariance) projected to a 2D ellipse on screen, the quad its 3-sigma box.
// Drawn back to front (the order buffer: sorted on the CPU, farthest first) with premultiplied alpha.
#extension GL_EXT_multiview : require

struct Splat {
    vec3 pos;           // model space
    uint cov01;         // covariance (model space): xx xy, xz yy, yz zz as half floats
    uint cov23;
    uint cov45;
    uint rgba;          // base colour (sRGB) and opacity, 8 bits each
    uint pad;
};
layout(std430, set = 0, binding = 0) readonly buffer Splats { Splat splats[]; };
layout(std430, set = 0, binding = 1) readonly buffer Order { uint order[]; };
layout(std140, set = 0, binding = 2) uniform Frame {
    mat4 clipFromWorld[2];      // both eyes (gl_ViewIndex)
    mat4 worldFromModel;        // the splat object's transform
    vec4 viewport;              // width, height (pixels per eye), 1/width, 1/height
} u;

layout(location = 0) out vec2 vSigma;     // the corner in standard deviations along the ellipse's axes
layout(location = 1) out vec4 vColor;

const vec2 kCorner[4] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(-1, 1), vec2(1, 1));   // a triangle strip
const float kExtent = 3.0;                                                            // the quad reaches 3 sigma

vec3 srgbToLinear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }

void main() {
    Splat s = splats[order[gl_InstanceIndex]];
    vec2 c01 = unpackHalf2x16(s.cov01), c23 = unpackHalf2x16(s.cov23), c45 = unpackHalf2x16(s.cov45);
    mat3 cov = mat3(c01.x, c01.y, c23.x,
                          c01.y, c23.y, c45.x,
                          c23.x, c45.x, c45.y);
    mat3 m3 = mat3(u.worldFromModel);
    mat3 covW = m3 * cov * transpose(m3);
    vec4 world = u.worldFromModel * vec4(s.pos, 1.0);
    mat4 cfw = u.clipFromWorld[gl_ViewIndex];
    vec4 clip = cfw * world;
    // behind the eye, or well off screen: no quad (a point outside the clip volume)
    if (clip.w <= 0.01 || any(greaterThan(abs(clip.xy), vec2(1.3 * clip.w)))) { gl_Position = vec4(0, 0, -2, 1); return; }
    // The projection's Jacobian at the centre: d(ndc.xy)/d(world), from the clip matrix's rows; in pixels.
    vec3 rx = vec3(cfw[0][0], cfw[1][0], cfw[2][0]), ry = vec3(cfw[0][1], cfw[1][1], cfw[2][1]), rw = vec3(cfw[0][3], cfw[1][3], cfw[2][3]);
    float iw = 1.0 / clip.w;
    vec3 jx = (rx - clip.x * iw * rw) * iw * (0.5 * u.viewport.x);
    vec3 jy = (ry - clip.y * iw * rw) * iw * (0.5 * u.viewport.y);
    // the screen-space covariance (pixels^2), with a little blur so a splat never shrinks below a pixel
    float a = dot(jx, covW * jx) + 0.3, b = dot(jx, covW * jy), d = dot(jy, covW * jy) + 0.3;
    float mid = 0.5 * (a + d), radius = sqrt(max(0.1, mid * mid - (a * d - b * b)));
    float l1 = mid + radius, l2 = max(0.1, mid - radius);
    vec2 axis = abs(b) > 1e-6 ? normalize(vec2(b, l1 - a)) : (a >= d ? vec2(1, 0) : vec2(0, 1));
    vec2 major = axis * sqrt(l1), minor = vec2(-axis.y, axis.x) * sqrt(l2);
    if (max(length(major), length(minor)) * kExtent < 0.5) { gl_Position = vec4(0, 0, -2, 1); return; }   // under half a pixel
    vec2 corner = kCorner[gl_VertexIndex];
    vec2 offsetPx = (corner.x * major + corner.y * minor) * kExtent;
    gl_Position = clip + vec4(offsetPx * 2.0 * u.viewport.zw * clip.w, 0, 0);
    vSigma = corner * kExtent;
    vec4 c = unpackUnorm4x8(s.rgba);
    vColor = vec4(srgbToLinear(c.rgb), c.a);
}
