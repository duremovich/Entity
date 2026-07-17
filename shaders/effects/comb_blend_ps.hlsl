// Blend combiner — composites input B over input A with a blend mode.
// Sockets: a = t0, b = t1.
//
// Param schema:
//   [0].x = mode     enum: 0 Normal, 1 Add, 2 Multiply, 3 Screen,
//                          4 Overlay, 5 Difference
//   [1].x = opacity  [0, 1] of the B contribution

#include "_effect_common.hlsli"

float3 overlayChannel(float3 a, float3 b) {
    // Per-component select (vector ternary isn't legal HLSL).
    float3 lo = 2.0f * a * b;
    float3 hi = 1.0f - 2.0f * (1.0f - a) * (1.0f - b);
    return lerp(hi, lo, step(a, 0.5f.xxx));
}

float4 PSMain(EffectVSOut i) : SV_TARGET {
    int   mode    = asint(g_params[0].x);  // Enum slots are bit-cast ints
    float opacity = saturate(g_params[1].x);

    float4 a = sampleInput(i.uv);
    float4 b = sampleInput1(i.uv);

    float3 blended;
    switch (mode) {
        case 1:  blended = a.rgb + b.rgb; break;              // Add
        case 2:  blended = a.rgb * b.rgb; break;              // Multiply
        case 3:  blended = 1.0f - (1.0f - a.rgb) * (1.0f - b.rgb); break;  // Screen
        case 4:  blended = overlayChannel(a.rgb, b.rgb); break;            // Overlay
        case 5:  blended = abs(a.rgb - b.rgb); break;         // Difference
        default: blended = b.rgb; break;                      // Normal (B over A)
    }

    // B's own alpha gates its contribution; opacity scales it.
    float w = saturate(b.a * opacity);
    return float4(lerp(a.rgb, blended, w), max(a.a, b.a * opacity));
}
