#version 450
// graph.glsl: a Customise recording track (or a preset's shape) drawn on the GPU at the screen's own resolution.
// Mode 0: a curve through the samples (Catmull-Rom), its glow, a gradient fill under it, a faint grid, the
// playhead (the part after it dimmer) and the point under the pointer. Mode 1: a colour track, its colours
// blended into one strip. Writes straight alpha; Bare premultiplies images when it draws them.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 3, binding = 0) uniform Params {
    vec4 u_size;    // width, height (pixels), samples, pixels per logical pixel
    vec4 u_c0;      // colour at the left; a: mode (0 curve, 1 colour strip)
    vec4 u_c1;      // colour at the right; a: playhead 0..1 (< 0: none)
    vec4 u_bg;      // background (straight alpha)
    vec4 u_misc;    // pointer 0..1 (< 0: none), grid strength (< 0: dark lines), line width, inset (logical px)
    vec4 u_v[60];   // heights 0..1, four to a vec4; mode 1: one colour to a vec4
};

int count() { return clamp(int(u_size.z), 1, 240); }

float height_at(int i) {
    i = clamp(i, 0, count() - 1);
    return u_v[i >> 2][i & 3];
}

// The curve at x (0..1): samples sit at the middle of their slots; the ends run flat.
float curve(float x) {
    float t = x * float(count()) - 0.5;
    float fi = floor(t), f = t - fi;
    int i = int(fi);
    float p0 = height_at(i - 1), p1 = height_at(i), p2 = height_at(i + 1), p3 = height_at(i + 2);
    float f2 = f * f, f3 = f2 * f;
    float y = 0.5 * (2.0 * p1 + (p2 - p0) * f + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * f2
                     + (3.0 * p1 - p0 - 3.0 * p2 + p3) * f3);
    return clamp(y, 0.0, 1.0);
}

vec4 over(vec4 d, vec4 s) {
    float a = s.a + d.a * (1.0 - s.a);
    vec3 c = (s.rgb * s.a + d.rgb * d.a * (1.0 - s.a)) / max(a, 1e-5);
    return vec4(c, a);
}

vec3 strip_colour(float x) {
    int n = min(count(), 60);
    float t = clamp(x * float(n) - 0.5, 0.0, float(n - 1));
    int i = int(floor(t));
    return mix(u_v[i].rgb, u_v[min(i + 1, n - 1)].rgb, t - float(i));
}

void main() {
    float W = u_size.x, H = u_size.y, s = max(u_size.w, 1.0);
    vec2 p = v_uv * u_size.xy;
    float play = u_c1.a, hover = u_misc.x;
    vec4 o = u_bg;

    if (u_c0.a > 0.5) {                     // a colour strip
        o = vec4(strip_colour(v_uv.x), 1.0);
        o.rgb *= 1.0 + 0.10 * (1.0 - v_uv.y) - 0.06;               // a little light from above
        if (play >= 0.0) {
            float dx = abs(p.x - play * W);
            o = over(o, vec4(1.0, 1.0, 1.0, clamp(1.5 * s - dx, 0.0, 1.0)));
            o = over(o, vec4(0.0, 0.0, 0.0, 0.25 * exp(-dx * dx / (18.0 * s * s)) * (1.0 - clamp(1.5 * s - dx, 0.0, 1.0))));
        }
        o_color = o;
        return;
    }

    float inset = u_misc.w * s, span = max(H - 2.0 * inset, 1.0);
    float cy = inset + (1.0 - curve(v_uv.x)) * span;
    float step = 1.0 / W;
    float slope = ((1.0 - curve(v_uv.x + step)) - (1.0 - curve(v_uv.x - step))) * 0.5 * span;
    float d = abs(p.y - cy) / sqrt(1.0 + slope * slope);
    vec3 col = mix(u_c0.rgb, u_c1.rgb, smoothstep(0.0, 1.0, v_uv.x));
    float ahead = play >= 0.0 && v_uv.x > play ? 0.5 : 1.0;      // not played yet this time round

    // the grid
    float g = u_misc.y;
    if (g != 0.0) {
        float gx = abs(fract(v_uv.x * 8.0 + 0.5) - 0.5) * W / 8.0;
        float gy = abs(fract((p.y - inset) / span * 4.0 + 0.5) - 0.5) * span / 4.0;
        float line = max(clamp(0.5 * s + 0.5 - gx, 0.0, 1.0), clamp(0.5 * s + 0.5 - gy, 0.0, 1.0));
        o = over(o, vec4(vec3(g > 0.0 ? 1.0 : 0.0), abs(g) * line));
    }
    // the fill under the curve, fading towards the bottom
    if (p.y > cy) {
        float k = clamp((p.y - cy) / max(H - cy, 1.0), 0.0, 1.0);
        o = over(o, vec4(col, mix(0.42, 0.02, k) * ahead));
    }
    // glow, then the line
    float lw = u_misc.z * s * 0.5;
    float glow = 5.0 * s;
    o = over(o, vec4(col, 0.5 * exp(-d * d / (2.0 * glow * glow)) * ahead));
    o = over(o, vec4(mix(col, vec3(1.0), 0.2), clamp(lw + 0.5 - d, 0.0, 1.0) * (ahead < 1.0 ? 0.7 : 1.0)));

    // the playhead: a faint line and a glowing point on the curve
    if (play >= 0.0) {
        float px = play * W;
        float dx = abs(p.x - px);
        o = over(o, vec4(1.0, 1.0, 1.0, 0.28 * clamp(0.75 * s + 0.5 - dx, 0.0, 1.0)));
        vec2 c = vec2(px, inset + (1.0 - curve(play)) * span);
        float r = length(p - c);
        o = over(o, vec4(col, 0.75 * exp(-r * r / (2.0 * 7.0 * 7.0 * s * s))));
        o = over(o, vec4(1.0, 1.0, 1.0, clamp(4.0 * s + 0.5 - r, 0.0, 1.0)));
    }
    // the point under the pointer: a ring
    if (hover >= 0.0) {
        float hx = hover * W;
        o = over(o, vec4(1.0, 1.0, 1.0, 0.16 * clamp(0.5 * s + 0.5 - abs(p.x - hx), 0.0, 1.0)));
        vec2 c = vec2(hx, inset + (1.0 - curve(hover)) * span);
        float r = length(p - c);
        float ring = clamp(1.25 * s + 0.5 - abs(r - 5.0 * s), 0.0, 1.0);
        o = over(o, vec4(col, clamp(5.0 * s - r, 0.0, 1.0) * 0.9));
        o = over(o, vec4(1.0, 1.0, 1.0, ring));
    }
    o_color = o;
}
