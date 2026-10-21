#version 450
// Frame Home's guide geometry (teleport arc, landing marker, seat markers): unlit, per-vertex colour, world space.
#extension GL_EXT_multiview : require
layout(location = 0) in vec3 position;
layout(location = 1) in vec4 color;
layout(push_constant) uniform Guide { mat4 clipFromWorld[2]; } pc;   // both eyes (gl_ViewIndex)
layout(constant_id = 0) const float kAlpha = 1.0;                    // the pass behind the scene draws faint
layout(location = 0) out vec4 vColor;
void main() {
    gl_Position = pc.clipFromWorld[gl_ViewIndex] * vec4(position, 1.0);
    vColor = vec4(color.rgb, color.a * kAlpha);
}
