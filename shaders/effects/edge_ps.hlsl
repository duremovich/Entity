// Edge detect — 3x3 Sobel operator on luminance. Outputs the gradient
// magnitude as a grayscale image; `threshold` clips out weak edges.
//
// Param schema:
//   [0].x = threshold  [0, 1]  edge magnitude below this becomes black
//   [1].x = thickness  [0.5, 4] pixel-sample distance for the kernel

#include "_effect_common.hlsli"

float luma(float3 rgb) {
    return dot(rgb, float3(0.2126f, 0.7152f, 0.0722f));
}

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float threshold = saturate(g_params[0].x);
    float thickness = max(g_params[1].x, 0.001f);

    float2 texel = (g_viewportSize.x > 0.0f && g_viewportSize.y > 0.0f)
                       ? (thickness / g_viewportSize)
                       : float2(thickness / 1920.0f, thickness / 1080.0f);

    // 3x3 Sobel taps.
    float tl = luma(sampleInput(i.uv + float2(-texel.x, -texel.y)).rgb);
    float t  = luma(sampleInput(i.uv + float2(0.0f,     -texel.y)).rgb);
    float tr = luma(sampleInput(i.uv + float2( texel.x, -texel.y)).rgb);
    float l  = luma(sampleInput(i.uv + float2(-texel.x,  0.0f)).rgb);
    float r  = luma(sampleInput(i.uv + float2( texel.x,  0.0f)).rgb);
    float bl = luma(sampleInput(i.uv + float2(-texel.x,  texel.y)).rgb);
    float b  = luma(sampleInput(i.uv + float2(0.0f,      texel.y)).rgb);
    float br = luma(sampleInput(i.uv + float2( texel.x,  texel.y)).rgb);

    float gx = -tl - 2.0f * l - bl + tr + 2.0f * r + br;
    float gy = -tl - 2.0f * t - tr + bl + 2.0f * b + br;
    float mag = sqrt(gx * gx + gy * gy);
    float e = (mag < threshold) ? 0.0f : saturate(mag);

    float4 src = sampleInput(i.uv);
    return float4(e, e, e, src.a);
}
