// Screen+ Upscaling "Sharp" on Plasma: the deblur of video.hlsl (two-radius unsharp mask on brightness, kept inside the
// neighbourhood's range so there are no halos) on the picture as Qt shows it on the display. Compiled to sharpen.frag.qsb with:
//   /usr/lib/qt6/bin/qsb --glsl "100 es,120,150" --hlsl 50 --msl 12 -o sharpen.frag.qsb sharpen.frag
#version 440
layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 texel;
};
layout(binding = 1) uniform sampler2D source;

vec3 T(float x, float y) { return texture(source, qt_TexCoord0 + vec2(x, y) * texel).rgb; }

void main()
{
    vec3 c = T(0.0, 0.0), n = T(0.0, -1.0), s = T(0.0, 1.0), w = T(-1.0, 0.0), e = T(1.0, 0.0);
    vec3 nw = T(-1.0, -1.0), se = T(1.0, 1.0), ne = T(1.0, -1.0), sw = T(-1.0, 1.0);
    vec3 n2 = T(0.0, -2.0), s2 = T(0.0, 2.0), w2 = T(-2.0, 0.0), e2 = T(2.0, 0.0);
    const vec3 L = vec3(0.2126, 0.7152, 0.0722);
    vec3 b1 = (c * 4.0 + (n + s + w + e) * 2.0 + (nw + ne + sw + se)) / 16.0;
    vec3 b2 = (b1 * 4.0 + n2 + s2 + w2 + e2) / 8.0;
    float d = dot(c - b1, L) * 1.6 + dot(b1 - b2, L) * 1.2;
    vec3 mn = min(c, min(min(min(n, s), min(w, e)), min(min(nw, se), min(ne, sw))));
    vec3 mx = max(c, max(max(max(n, s), max(w, e)), max(max(nw, se), max(ne, sw))));
    fragColor = vec4(clamp(clamp(c + d, mn - 0.02, mx + 0.02), 0.0, 1.0), 1.0) * qt_Opacity;
}
