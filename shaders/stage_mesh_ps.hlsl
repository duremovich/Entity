Texture2D    g_tex : register(t0);
SamplerState g_smp : register(s0);

cbuffer DrawCB : register(b1) {
    float4x4 model;
    float4   tint;
    uint     renderMode;
    uint3    _pad;
};

struct PSIn { float4 sv : SV_POSITION; float2 uv : TEXCOORD0; float3 wn : NORMAL; };

float4 PSMain(PSIn i) : SV_TARGET {
    if (renderMode == 0u) {
        return g_tex.Sample(g_smp, i.uv);
    }
    float3 L = normalize(float3(0.5, 1.0, 0.3));
    float  ndotl = saturate(dot(normalize(i.wn), L));
    float  shade = 0.35 + 0.65 * ndotl;
    return float4(tint.rgb * shade, tint.a);
}
