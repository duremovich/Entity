// Sharpen — unsharp mask. Subtract a single-step blur from the centre
// sample and add it back scaled by `amount`. Cheap, decent results at
// moderate sharpen levels; over-sharpening at amount > 1 will produce
// halo artefacts (by design — that's what unsharp masks do).
//
// Param schema:
//   [0].x = amount  [0, 4]   0 = passthrough, 1 = subtle, 2-3 = obvious
//   [1].x = radius  [0.1, 4] pixel offset for the surround taps

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float amount = max(g_params[0].x, 0.0f);
    float radius = max(g_params[1].x, 0.001f);

    if (amount < 0.001f) return sampleInput(i.uv);

    float2 texel = (g_viewportSize.x > 0.0f && g_viewportSize.y > 0.0f)
                       ? (radius / g_viewportSize)
                       : float2(radius / 1920.0f, radius / 1080.0f);

    float4 c  = sampleInput(i.uv);
    float4 t  = sampleInput(i.uv + float2(0.0f, -texel.y));
    float4 b  = sampleInput(i.uv + float2(0.0f,  texel.y));
    float4 l  = sampleInput(i.uv + float2(-texel.x, 0.0f));
    float4 r  = sampleInput(i.uv + float2( texel.x, 0.0f));
    float4 surround = (t + b + l + r) * 0.25f;

    float4 high = c - surround;
    c.rgb = saturate(c.rgb + high.rgb * amount);
    return c;
}
