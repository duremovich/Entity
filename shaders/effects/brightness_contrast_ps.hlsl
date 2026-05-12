// Brightness + contrast adjustment. Simple linear ops on RGB; alpha
// passes through unchanged.
//
// Param schema (positional, matches EffectKindRegistry::registerBuiltins):
//   [0].x = brightness  [-1, 1]
//   [1].x = contrast    [-1, 1]

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float4 c = sampleInput(i.uv);
    float brightness = g_params[0].x;
    float contrast   = g_params[1].x;
    // Contrast: pivot at 0.5; >0 stretches, <0 compresses. Brightness is
    // a simple additive offset after the contrast op.
    c.rgb = (c.rgb - 0.5f) * (1.0f + contrast) + 0.5f + brightness;
    return c;
}
