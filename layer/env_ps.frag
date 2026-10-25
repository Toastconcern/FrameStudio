#version 450
// Frame Home's environment, drawn by vrcompositor in place of Aurora (frame_home_layer.cpp swaps it in for every
// Aurora pipeline: same vertex shader, same set 0, plus set 1 below).
//
// Aurora's vertex shader hands over each point's tracking-space position (location 1) and the frame's uniform block
// holds the eye position (_m11, as Aurora's floor shader reads it). The direction from the eye to the point is
// looked up in the newest frame Frame Home published: its projection * view rotation for that eye, so the picture
// stays locked to the world as the head turns between Frame Home's frame and the compositor's (rotation reprojection).
// A frame shown again (half rate, or the next one late) is carried on by how the rig was moving (env_share.h Motion):
// its smooth turn exactly, its walking as if everything were walk.w metres away.

// SteamVR's per-frame block (set 0, binding 4), as far as the members used here.
layout(set = 0, binding = 4, std140) uniform CompositorFrame {
    layout(row_major) mat4 m0, m1, m2, m3, m4;
    vec4 m5[4];
    vec4 m6;        // w: the environment's opacity (the compositor dims it behind the menu and in transitions)
    vec4 m7;
    vec2 m8, m9;
    vec4 m10;
    vec4 m11;       // xyz: this eye's position, tracking space
} frame;

layout(set = 1, binding = 0) uniform sampler2DArray home;          // both eyes, layer 0 left, 1 right
layout(set = 1, binding = 1, std140) uniform HomeFrame {
    mat4 viewProj[2];   // projection * view rotation, per eye (tracking space)
    vec4 eyePos[2];     // where each eye was when Frame Home rendered it
    vec4 params;        // x: brightness (0 fades to black), y: 1 when there's something to show
    vec4 turn;          // xy: cos, sin of the rig's turn since the frame (about tracking-space up)
    vec4 walk;          // xyz: the rig's walk since the frame (frame's tracking space, m), w: depth taken (0: none)
} homeFrame;

layout(location = 1) in vec3 worldPos;
layout(location = 0) out vec4 color;

void main() {
    const vec3 eye = frame.m11.xyz;
    vec3 dir = normalize(worldPos - eye);
    const int k = distance(eye, homeFrame.eyePos[0].xyz) <= distance(eye, homeFrame.eyePos[1].xyz) ? 0 : 1;
    // Into the frame's tracking space: the rig turned since by this much (about the head), and walked.
    const vec2 cs = homeFrame.turn.xy;
    dir = vec3(cs.x * dir.x + cs.y * dir.z, dir.y, -cs.y * dir.x + cs.x * dir.z);
    if (homeFrame.walk.w > 0.0) dir = normalize(dir * homeFrame.walk.w + homeFrame.walk.xyz);
    const vec4 c = homeFrame.viewProj[k] * vec4(dir, 0.0);
    vec3 rgb = vec3(0.0);
    if (c.w > 1e-5 && homeFrame.params.y > 0.5) {
        const vec2 uv = (c.xy / c.w) * 0.5 + 0.5;
        // (explicit level 0: subsampled frames must be read so, and plain ones have only that level)
        rgb = textureLod(home, vec3(uv, float(k)), 0.0).rgb;
    }
    color = vec4(rgb * homeFrame.params.x, frame.m6.w);
}
