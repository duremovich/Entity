// Checkerboard generator — alternating two-color grid. Never samples
// g_input (zero texture-input sockets).
//
// Param schema:
//   [0].x    = cols (number of columns, >= 1)
//   [1].x    = rows (number of rows, >= 1)
//   [2].xyzw = colorA (linear RGBA)
//   [3].xyzw = colorB (linear RGBA)

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float cols = max(1.0f, g_params[0].x);
    float rows = max(1.0f, g_params[1].x);
    float4 colorA = g_params[2];
    float4 colorB = g_params[3];

    int cx = (int)floor(i.uv.x * cols);
    int cy = (int)floor(i.uv.y * rows);
    bool odd = ((cx + cy) & 1) != 0;
    return odd ? colorB : colorA;
}
