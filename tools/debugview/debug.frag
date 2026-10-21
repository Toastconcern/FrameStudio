#version 450
// Shaded (spec 0 = 1): faceted from the surface's own slope, so shapes read without any lighting; else flat.
layout(constant_id = 0) const int kShade = 1;
layout(location = 0) in vec3 vWorld;
layout(location = 1) flat in vec4 vColor;
layout(location = 0) out vec4 outColor;
void main() {
    vec3 c = vColor.rgb;
    if (kShade == 1) {
        vec3 n = cross(dFdx(vWorld), dFdy(vWorld));
        float l = dot(n, n) > 1e-20 ? abs(dot(normalize(n), normalize(vec3(0.35, 0.85, 0.4)))) : 1.0;
        c *= 0.42 + 0.58 * l;
    }
    outColor = vec4(c, vColor.a);
}
