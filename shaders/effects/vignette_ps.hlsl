// Vignette — radial darkening anchored at the centre of the layer.
//
// Param schema:
//   [0].x = radius     [0, 1]  smaller = tighter dark vignette
//   [1].x = softness   [0, 1]  falloff width
//   [2].x = intensity  [0, 1]  0 = off, 1 = full black at the edge

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float4 c = sampleInput(i.uv);
    float radius    = saturate(g_params[0].x);
    float softness  = saturate(g_params[1].x);
    float intensity = saturate(g_params[2].x);

    // Distance from centre normalised by half-diagonal.
    float2 d = i.uv - 0.5f;
    float r = length(d) / 0.70710678f;  // 1/sqrt(2) — corner distance

    float inner = radius;
    float outer = saturate(radius + softness + 0.0001f);
    float mask  = smoothstep(inner, outer, r);
    c.rgb *= lerp(1.0f, 1.0f - intensity, mask);
    return c;
}
