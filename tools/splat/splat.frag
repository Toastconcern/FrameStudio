#version 450
// A splat's footprint: the Gaussian's falloff, exp(-r^2 / 2) with r in standard deviations, times its opacity.
// Premultiplied output (blend ONE, ONE_MINUS_SRC_ALPHA): splats drawn far to near.
layout(location = 0) in vec2 vSigma;
layout(location = 1) in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() {
    float r2 = dot(vSigma, vSigma);
    if (r2 > 9.0) discard;                                   // outside 3 sigma: nothing worth blending
    float alpha = min(0.99, vColor.a * exp(-0.5 * r2));
    if (alpha < 1.0 / 255.0) discard;
    outColor = vec4(vColor.rgb * alpha, alpha);
}
