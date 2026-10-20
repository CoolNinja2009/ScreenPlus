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
    return float4(yuv_to_rgb(float3(texY.Sample(smp, c), texUV.Sample(smp, c))), 1.0);
}

float4 ps_i420(VSOut i) : SV_Target
{
    float3 c = coord(i.uv);
    return float4(yuv_to_rgb(float3(texY.Sample(smp, c), texU.Sample(smp, c), texV.Sample(smp, c))), 1.0);
}

float4 ps_rgb(VSOut i) : SV_Target
{
    return float4(texRGB.Sample(smp, coord(i.uv)).rgb, 1.0);
}
