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

float3 coord(float2 uv)
{
    return float3(min(uvRect.xy + uv * uvRect.zw, uvClamp.xy), off.w);
}

float3 yuv_to_rgb(float3 yuv)
{
    yuv -= off.xyz;
    return saturate(float3(dot(m0.xyz, yuv), dot(m1.xyz, yuv), dot(m2.xyz, yuv)));
}

float4 ps_nv12(VSOut i) : SV_Target
{
    float3 c = coord(i.uv);
    return float4(apply_grade(yuv_to_rgb(float3(texY.Sample(smp, c), texUV.Sample(smp, c)))), 1.0);
}

float4 ps_i420(VSOut i) : SV_Target
{
    float3 c = coord(i.uv);
    return float4(apply_grade(yuv_to_rgb(float3(texY.Sample(smp, c), texU.Sample(smp, c), texV.Sample(smp, c)))), 1.0);
}

float4 ps_rgb(VSOut i) : SV_Target
{
    return float4(apply_grade(texRGB.Sample(smp, coord(i.uv)).rgb), 1.0);
}
