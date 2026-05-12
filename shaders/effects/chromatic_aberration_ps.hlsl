// Chromatic aberration — shift the R and B channels in opposite
// directions along the screen-space radial vector. Mimics a cheap
// lens artefact / VHS look.
//
// Param schema:
//   [0].x = amount  [0, 32]  pixels of channel separation at the edges

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float amount = g_params[0].x;
    if (abs(amount) < 0.001f) return sampleInput(i.uv);

    float2 texel = (g_viewportSize.x > 0.0f && g_viewportSize.y > 0.0f)
                       ? (1.0f / g_viewportSize)
                       : float2(1.0f / 1920.0f, 1.0f / 1080.0f);

    // Radial offset: zero at centre, full at corners. Each channel rides
    // the same axis but mirrored: R pulled outward, B pushed inward.
    float2 radial = i.uv - 0.5f;
    float2 offset = radial * amount;
    float2 shift  = offset * texel;

    float r = sampleInput(i.uv + shift).r;
    float g = sampleInput(i.uv).g;
    float b = sampleInput(i.uv - shift).b;
    float a = sampleInput(i.uv).a;
    return float4(r, g, b, a);
}
