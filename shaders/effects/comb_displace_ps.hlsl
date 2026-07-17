// Displace combiner — offsets input A's sample position by input B's
// red/green channels (0.5 = no offset). Pair with Fractal Noise for heat
// shimmer / water; with a gradient for smears.
// Sockets: a = t0, map = t1.
//
// Param schema:
//   [0].x = amountX  max horizontal offset in pixels
//   [1].x = amountY  max vertical offset in pixels

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float amountX = g_params[0].x;
    float amountY = g_params[1].x;

    float4 map = sampleInput1(i.uv);
    float2 texel = (g_viewportSize.x > 0.0f && g_viewportSize.y > 0.0f)
        ? 1.0f / g_viewportSize
        : float2(0.0f, 0.0f);
    float2 offset = float2((map.r - 0.5f) * 2.0f * amountX * texel.x,
                           (map.g - 0.5f) * 2.0f * amountY * texel.y);

    return sampleInput(saturate(i.uv + offset));
}
