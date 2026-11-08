// Screen+ video presentation shaders (compiled at build time with fxc, shader model 4.0).
// A single full-screen triangle samples the decoded frame with cover-crop texture coordinates
// and converts YUV to RGB using the frame's matrix/range (supplied in the constant buffer).

cbuffer Params : register(b0)
{
    float4 uvRect;   // xy = offset, zw = scale (texture space)
    float4 uvClamp;  // xy = max coordinate (avoids sampling decoder padding)
    float4 m0;       // rgb = M * (yuv - off)
    float4 m1;
    float4 m2;
    float4 off;      // xyz = yuv offset, w = texture array slice
    float4 grade;    // x brightness offset, y contrast gain, z gamma exponent, w saturation gain
    float4 hue;      // x hue rotation, radians; yzw unused
    float4 tint;     // xyz tint rgb (0..1, includes tint brightness), w tint lift (0..1)
    float4 up;       // x upscale: 0 bilinear, 1 bicubic, 2 bicubic + sharpen, 3 sharpen only; y sharpness 0..1; zw texel size
};

Texture2DArray<float>  texY   : register(t0);
Texture2DArray<float2> texUV  : register(t1);
Texture2DArray<float>  texU   : register(t2);
Texture2DArray<float>  texV   : register(t3);
Texture2DArray<float4> texRGB : register(t4);
SamplerState smp : register(s0);

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

/* Screen+ colour grading (Customise panel): brightness/contrast/gamma, saturation,
 * hue rotation, then a tint overlay that lifts blacks towards the tint colour. */
float3 hue_rotate(float3 c, float a)
{
    const float3x3 toYIQ = { 0.299,  0.587,    0.114,
                             0.595916, -0.274556, -0.284113,
                             0.211456, -0.522591, 0.311135 };
    const float3x3 toRGB = { 1, 0.956295, 0.621024,
                             1, -0.272121, -0.637934,
                             1, -1.106843, 1.704585 };
    float3 yiq = mul(toYIQ, c);
    float2 rot = float2(yiq.y * cos(a) - yiq.z * sin(a), yiq.y * sin(a) + yiq.z * cos(a));
    return mul(toRGB, float3(yiq.x, rot));
}

float3 apply_grade(float3 c)
{
    c += grade.x;
    c = (c - 0.5) * grade.y + 0.5;
    c = pow(saturate(max(c, 0.0)), grade.z);
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = luma + (c - luma) * grade.w;
    if (abs(hue.x) > 0.001) c = hue_rotate(c, hue.x);
    float3 lifted = max(c, tint.rgb);
    c = lerp(c, lifted, tint.w);
    return saturate(c);
}

VSOut vs_main(uint id : SV_VertexID)
{
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = t;
    return o;
}

float3 yuv_to_rgb(float3 yuv)
{
    yuv -= off.xyz;
    return saturate(float3(dot(m0.xyz, yuv), dot(m1.xyz, yuv), dot(m2.xyz, yuv)));
}

// One source sample as RGB. fmt is a literal at every call, so each shader keeps only its own branch.
float3 fetch(int fmt, float2 t)
{
    float3 c = float3(min(max(t, 0.0), uvClamp.xy), off.w);
    if (fmt == 0) return yuv_to_rgb(float3(texY.Sample(smp, c), texUV.Sample(smp, c)));
    if (fmt == 1) return yuv_to_rgb(float3(texY.Sample(smp, c), texU.Sample(smp, c), texV.Sample(smp, c)));
    return texRGB.Sample(smp, c).rgb;
}

// Catmull-Rom bicubic in 9 bilinear taps (the weights of the two middle texels folded into one tap).
float3 bicubic(int fmt, float2 t)
{
    float2 size = 1.0 / up.zw;
    float2 p = t * size;
    float2 p1 = floor(p - 0.5) + 0.5;
    float2 f = p - p1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 t0 = (p1 - 1.0) * up.zw, t3 = (p1 + 2.0) * up.zw, t12 = (p1 + w2 / w12) * up.zw;
    float3 r = 0;
    r += fetch(fmt, float2(t0.x,  t0.y))  * w0.x  * w0.y;
    r += fetch(fmt, float2(t12.x, t0.y))  * w12.x * w0.y;
    r += fetch(fmt, float2(t3.x,  t0.y))  * w3.x  * w0.y;
    r += fetch(fmt, float2(t0.x,  t12.y)) * w0.x  * w12.y;
    r += fetch(fmt, float2(t12.x, t12.y)) * w12.x * w12.y;
    r += fetch(fmt, float2(t3.x,  t12.y)) * w3.x  * w12.y;
    r += fetch(fmt, float2(t0.x,  t3.y))  * w0.x  * w3.y;
    r += fetch(fmt, float2(t12.x, t3.y))  * w12.x * w3.y;
    r += fetch(fmt, float2(t3.x,  t3.y))  * w3.x  * w3.y;
    return saturate(r);
}

// Deblur: a two-radius unsharp mask on brightness (fine detail one texel out, the wider softness of an
// enlarged or soft picture two out), kept inside the 3x3 neighbourhood's range so edges get no halos and
// colours no fringes. up.y is the strength.
float3 sharpen(int fmt, float2 t, float3 c)
{
    float2 d = up.zw;
    float3 n  = fetch(fmt, t - float2(0, d.y)), s  = fetch(fmt, t + float2(0, d.y));
    float3 w  = fetch(fmt, t - float2(d.x, 0)), e  = fetch(fmt, t + float2(d.x, 0));
    float3 nw = fetch(fmt, t - d),              se = fetch(fmt, t + d);
    float3 ne = fetch(fmt, t + float2(d.x, -d.y)), sw = fetch(fmt, t + float2(-d.x, d.y));
    float3 n2 = fetch(fmt, t - float2(0, 2 * d.y)), s2 = fetch(fmt, t + float2(0, 2 * d.y));
    float3 w2 = fetch(fmt, t - float2(2 * d.x, 0)), e2 = fetch(fmt, t + float2(2 * d.x, 0));
    const float3 L = float3(0.2126, 0.7152, 0.0722);
    float3 blur1 = (c * 4.0 + (n + s + w + e) * 2.0 + (nw + ne + sw + se)) / 16.0;
    float3 blur2 = (blur1 * 4.0 + n2 + s2 + w2 + e2) / 8.0;
    float detail = dot(c - blur1, L) * 1.6 + dot(blur1 - blur2, L) * 1.2;
    float3 mn = min(min(min(n, s), min(w, e)), min(min(nw, se), min(ne, sw)));
    float3 mx = max(max(max(n, s), max(w, e)), max(max(nw, se), max(ne, sw)));
    mn = min(mn, c); mx = max(mx, c);
    float3 r = c + detail * up.y;
    return saturate(clamp(r, mn - 0.02, mx + 0.02));
}

float3 picture(int fmt, float2 uv)
{
    float2 t = uvRect.xy + uv * uvRect.zw;
    float3 c;
    if (up.x < 0.5 || up.x > 2.5) c = fetch(fmt, t);
    else c = bicubic(fmt, t);
    if (up.x > 1.5) c = sharpen(fmt, t, c);
    return c;
}

float4 ps_nv12(VSOut i) : SV_Target
{
    return float4(apply_grade(picture(0, i.uv)), 1.0);
}

float4 ps_i420(VSOut i) : SV_Target
{
    return float4(apply_grade(picture(1, i.uv)), 1.0);
}

float4 ps_rgb(VSOut i) : SV_Target
{
    return float4(apply_grade(picture(2, i.uv)), 1.0);
}
