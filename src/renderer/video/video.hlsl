// Screen+ video presentation shaders (compiled at build time with fxc, shader model 4.0).
// A single full-screen triangle places the decoded frame at the Customise layout's rect (cover by default)
// and converts YUV to RGB using the frame's matrix/range (supplied in the constant buffer). Outside the rect
// the fill shows: a colour, the picture mirrored or its edges stretched (free: folded or clamped coordinates),
// or a blur, which costs two tiny passes (the picture covering the display drawn at 1/8 size, then 1/32, a
// dual-filter style downsample) and a 9-tap tent on that, only where the border shows.

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
                     // (blur source pass: zw = the tap offset)
    float4 lay;      // the picture's rect on the output (uv): xy offset, zw size
    float4 fill;     // x fill (0 colour, 1 blur, 2 mirror, 3 edges); y pass (0 draw, 1 blur source); zw texBlur texel
    float4 fillColor;// rgb the colour; w the blur's dimming
};

Texture2DArray<float>  texY   : register(t0);
Texture2DArray<float2> texUV  : register(t1);
Texture2DArray<float>  texU   : register(t2);
Texture2DArray<float>  texV   : register(t3);
Texture2DArray<float4> texRGB : register(t4);
Texture2D<float4>      texBlur: register(t5);
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

float3 picture(int fmt, float2 q)
{
    float2 t = uvRect.xy + q * uvRect.zw;
    float3 c;
    if (up.x < 0.5 || up.x > 2.5) c = fetch(fmt, t);
    else c = bicubic(fmt, t);
    if (up.x > 1.5) c = sharpen(fmt, t, c);
    return c;
}

// the blurred cover of the picture: a tent over the 1/32 copy
float3 blurred(float2 uv)
{
    float2 d = fill.zw;
    float3 c = texBlur.SampleLevel(smp, uv, 0).rgb * 4.0;
    c += (texBlur.SampleLevel(smp, uv + float2(d.x, 0), 0) + texBlur.SampleLevel(smp, uv - float2(d.x, 0), 0) +
          texBlur.SampleLevel(smp, uv + float2(0, d.y), 0) + texBlur.SampleLevel(smp, uv - float2(0, d.y), 0)).rgb * 2.0;
    c += (texBlur.SampleLevel(smp, uv + d, 0) + texBlur.SampleLevel(smp, uv - d, 0) +
          texBlur.SampleLevel(smp, uv + float2(d.x, -d.y), 0) + texBlur.SampleLevel(smp, uv + float2(-d.x, d.y), 0)).rgb;
    return c / 16.0;
}

float4 shade(int fmt, float2 uv)
{
    float2 q = (uv - lay.xy) / lay.zw;
    float3 c;
    if (fill.y > 0.5) {                                   // blur source: four taps of the cover picture
        float2 t = uvRect.xy + q * uvRect.zw, o = up.zw;
        c = (fetch(fmt, t - o) + fetch(fmt, t + o) + fetch(fmt, t + float2(o.x, -o.y)) + fetch(fmt, t + float2(-o.x, o.y))) * 0.25;
    } else {
        bool outside = any(q < 0.0) || any(q > 1.0);
        if (outside && fill.x > 2.5) q = saturate(q);
        else if (outside && fill.x > 1.5) q = abs(q - 2.0 * floor(q * 0.5 + 0.5));
        if (!outside || fill.x > 1.5) c = apply_grade(picture(fmt, q));
        else if (fill.x > 0.5) c = apply_grade(blurred(uv)) * fillColor.w;
        else c = fillColor.rgb;
    }
    return float4(c, 1.0);
}

float4 ps_nv12(VSOut i) : SV_Target
{
    return shade(0, i.uv);
}

float4 ps_i420(VSOut i) : SV_Target
{
    return shade(1, i.uv);
}

float4 ps_rgb(VSOut i) : SV_Target
{
    return shade(2, i.uv);
}

// blur chain: 1/8 -> 1/32, four bilinear taps half a source texel out (each averages 4x4 texels)
float4 ps_down(VSOut i) : SV_Target
{
    float2 d = fill.zw;
    float3 c = texBlur.SampleLevel(smp, i.uv - d, 0).rgb + texBlur.SampleLevel(smp, i.uv + d, 0).rgb +
               texBlur.SampleLevel(smp, i.uv + float2(d.x, -d.y), 0).rgb + texBlur.SampleLevel(smp, i.uv + float2(-d.x, d.y), 0).rgb;
    return float4(c * 0.25, 1.0);
}
