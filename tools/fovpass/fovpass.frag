#version 450
#extension GL_EXT_multiview : require
// The foveated frame, read through a subsampled sampler: where it was drawn at less than full detail the driver
// filters between its coarse pixels, so the edges of view come out smooth instead of in blocks. Sampled at each
// pixel's centre: at full detail it returns the drawn pixel (to within the sRGB round trip's last bit).
layout(set = 0, binding = 0) uniform sampler2DArray frame;
layout(push_constant) uniform Size { vec2 inv; } size;
layout(location = 0) out vec4 color;
void main() {
    color = textureLod(frame, vec3(gl_FragCoord.xy * size.inv, float(gl_ViewIndex)), 0.0);
}
