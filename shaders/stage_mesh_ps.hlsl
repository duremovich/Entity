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
    float4 c;
    if (renderMode == 0u) {
        // Multiply by tint so callers can fade textured screens (stage-view
        // opacity). Textured callers default tint=(1,1,1,1) — no visual change.
        c = g_tex.Sample(g_smp, i.uv) * tint;
    } else {
        float3 L = normalize(float3(0.5, 1.0, 0.3));
        float  ndotl = saturate(dot(normalize(i.wn), L));
        float  shade = 0.35 + 0.65 * ndotl;
        c = float4(tint.rgb * shade, tint.a);
    }
    // Premultiply so the transparent PSO's premul-over blend composites
    // correctly within the stage RT (multiple scrims, transparent over
    // opaque). A final de-premul fullscreen pass restores straight alpha
    // for ImGui's downstream composite. For a=1 this is a no-op, so the
    // opaque-only path is byte-identical to before.
    c.rgb *= c.a;
    return c;
}
