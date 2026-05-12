// Invert — colour inversion mixed against the source by `amount`.
// amount=0 → passthrough; amount=1 → full negative.
//
// Param schema:
//   [0].x = amount  [0, 1]

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float amount = saturate(g_params[0].x);
    float4 c = sampleInput(i.uv);
    float3 inv = 1.0f - c.rgb;
    c.rgb = lerp(c.rgb, inv, amount);
    return c;
}
