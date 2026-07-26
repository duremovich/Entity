// Plasma generator — classic sine-interference field blended between two
// colors. Never samples g_input (zero texture-input sockets).
//
// Param schema:
//   [0].x    = scale  interference frequency
//   [1].x    = speed  evolve rate (uses g_timeSeconds; 0 = static)
//   [2].xyzw = colorA (linear RGBA)
//   [3].xyzw = colorB (linear RGBA)

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float scale = max(0.1f, g_params[0].x);
    float t     = g_params[1].x * g_timeSeconds;
    float4 colorA = g_params[2];
    float4 colorB = g_params[3];

    float2 p = i.uv * scale;
    float v = sin(p.x + t)
            + sin((p.y + t) * 0.7f)
            + sin((p.x + p.y + t) * 0.6f)
            + sin(length(p - scale * 0.5f) * 1.3f - t);
    v = 0.5f + 0.5f * sin(v * 1.57079632f);  // fold into [0, 1]

    return lerp(colorA, colorB, v);
}
