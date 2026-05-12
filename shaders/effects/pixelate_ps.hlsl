// Pixelate — snap UV to a coarse grid before sampling. The grid is sized
// in pixels of the input texture (via g_viewportSize from the renderer).
//
// Param schema:
//   [0].x = pixel_size  [1, 128]  width/height in pixels of one output cell

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float pixelSize = max(g_params[0].x, 1.0f);

    float2 dims = (g_viewportSize.x > 0.0f && g_viewportSize.y > 0.0f)
                      ? g_viewportSize
                      : float2(1920.0f, 1080.0f);
    float2 cellsPerScreen = dims / pixelSize;

    // Snap UV to the nearest cell centre.
    float2 cell = floor(i.uv * cellsPerScreen) + 0.5f;
    float2 uv   = cell / cellsPerScreen;
    return sampleInput(uv);
}
