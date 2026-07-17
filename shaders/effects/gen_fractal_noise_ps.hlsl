// Fractal Noise generator — grayscale value-noise FBM, the workhorse
// procedural texture (displacement source, luma mask, organic fill).
// Never samples g_input (zero texture-input sockets).
//
// Param schema:
//   [0].x = scale     base frequency (cells across the frame)
//   [1].x = octaves   [1, 8] FBM layers (fractional part ignored)
//   [2].x = speed     scroll/evolve rate (uses g_timeSeconds; 0 = static)
//   [3].x = contrast  [0, 4] post-curve around 0.5

#include "_effect_common.hlsli"

// Deterministic hash-based value noise (no texture lookups so results
// are identical across GPUs modulo float rounding).
float hash21(float2 p) {
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float valueNoise(float2 p) {
    float2 cell = floor(p);
    float2 f    = frac(p);
    f = f * f * (3.0f - 2.0f * f);  // smoothstep fade
    float a = hash21(cell);
    float b = hash21(cell + float2(1, 0));
    float c = hash21(cell + float2(0, 1));
    float d = hash21(cell + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float scale    = max(0.01f, g_params[0].x);
    int   octaves  = clamp((int)g_params[1].x, 1, 8);
    float speed    = g_params[2].x;
    float contrast = max(0.0f, g_params[3].x);

    float2 p = i.uv * scale + speed * g_timeSeconds * float2(0.31f, 0.17f);

    float v = 0.0f;
    float amp = 0.5f;
    for (int o = 0; o < 8; ++o) {
        if (o >= octaves) break;
        v += valueNoise(p) * amp;
        p *= 2.02f;
        amp *= 0.5f;
    }

    v = saturate((v - 0.5f) * contrast + 0.5f);
    return float4(v, v, v, 1.0f);
}
