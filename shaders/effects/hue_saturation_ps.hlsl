// Hue / saturation / lightness shift. Operates in HSL space converted
// from RGB; clamps results to the [0, 1] cube on output.
//
// Param schema:
//   [0].x = hue        degrees, wraps at ±180
//   [1].x = saturation [-1, 1] additive (negative = desaturate)
//   [2].x = lightness  [-1, 1] additive

#include "_effect_common.hlsli"

float3 rgbToHsl(float3 c) {
    float maxc = max(c.r, max(c.g, c.b));
    float minc = min(c.r, min(c.g, c.b));
    float l    = (maxc + minc) * 0.5f;
    float d    = maxc - minc;
    float h    = 0.0f, s = 0.0f;
    if (d > 0.0001f) {
        s = (l < 0.5f) ? d / (maxc + minc) : d / (2.0f - maxc - minc);
        if (maxc == c.r) {
            h = (c.g - c.b) / d + ((c.g < c.b) ? 6.0f : 0.0f);
        } else if (maxc == c.g) {
            h = (c.b - c.r) / d + 2.0f;
        } else {
            h = (c.r - c.g) / d + 4.0f;
        }
        h /= 6.0f;
    }
    return float3(h, s, l);
}

float hueToChannel(float p, float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f)        return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

float3 hslToRgb(float3 hsl) {
    float h = hsl.x, s = hsl.y, l = hsl.z;
    if (s < 0.0001f) return float3(l, l, l);
    float q = (l < 0.5f) ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    return float3(
        hueToChannel(p, q, h + 1.0f / 3.0f),
        hueToChannel(p, q, h),
        hueToChannel(p, q, h - 1.0f / 3.0f)
    );
}

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float4 c = sampleInput(i.uv);
    float hueShift   = g_params[0].x / 360.0f;  // degrees → [0, 1]
    float satOffset  = g_params[1].x;
    float lightShift = g_params[2].x;

    float3 hsl = rgbToHsl(saturate(c.rgb));
    hsl.x = frac(hsl.x + hueShift + 1.0f);   // wrap into [0, 1]
    hsl.y = saturate(hsl.y + satOffset);
    hsl.z = saturate(hsl.z + lightShift);

    c.rgb = saturate(hslToRgb(hsl));
    return c;
}
