#version 450
// Debug views (HslRenderer::setDebugView): any mesh, drawn by its position alone in a flat colour per draw.
#extension GL_EXT_multiview : require
layout(location = 0) in vec3 position;
struct Draw { mat4 clipFromModel[2]; mat4 worldFromModel; vec4 color; };
layout(std430, set = 0, binding = 0) readonly buffer Draws { Draw d[]; } draws;   // one per draw (firstInstance)
layout(location = 0) out vec3 vWorld;
layout(location = 1) flat out vec4 vColor;
void main() {
    gl_Position = draws.d[gl_InstanceIndex].clipFromModel[gl_ViewIndex] * vec4(position, 1.0);
    vWorld = (draws.d[gl_InstanceIndex].worldFromModel * vec4(position, 1.0)).xyz;
    vColor = draws.d[gl_InstanceIndex].color;
}
