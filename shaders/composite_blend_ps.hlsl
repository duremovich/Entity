/**
 * Composite Blend Pixel Shader
 *
 * Handles the 9 blend modes that D3D12 fixed-function blend state can't
 * express (Overlay, SoftLight, HardLight, ColorDodge, ColorBurn, Darken,
 * Lighten, Difference, Exclusion).
 *
 * Samples both the foreground clip texture (t0) and a snapshot of the current
 * compose target (t1) taken just before this draw. Computes the blend in HLSL
 * and writes the final color directly. The PSO this shader runs in has
 * BlendEnable=FALSE so the output replaces the destination outright.
 *
 * The bg snapshot is sampled at the current screen-space pixel (via
 * SV_POSITION / texture dimensions), NOT via the fg's input UVs - the fg may
 * be transformed/scaled relative to the compose target, but the bg pixel
 * we're blending with is always the one we're writing to.
 */

#include "common.hlsli"

Texture2D fgTexture : register(t0);
Texture2D bgTexture : register(t1);
SamplerState samplerState : register(s0);

float3 blendOverlay(float3 fg, float3 bg) {
    // Per-channel branch: HLSL's ?: needs a scalar condition for vectors,
    // so use step(0.5, bg) (0 where bg<0.5, 1 where bg>=0.5) + lerp.
    float3 mask = step(0.5, bg);
    return lerp(2.0 * fg * bg, 1.0 - 2.0 * (1.0 - fg) * (1.0 - bg), mask);
}

float3 blendHardLight(float3 fg, float3 bg) {
    // Overlay with operands swapped.
    return blendOverlay(bg, fg);
}

float3 blendSoftLight(float3 fg, float3 bg) {
    // Pegtop variant - smooth, matches the formula listed in the W3C spec.
    return (1.0 - 2.0 * fg) * bg * bg + 2.0 * fg * bg;
}

float3 blendColorDodge(float3 fg, float3 bg) {
    // bg / (1 - fg), with fg>=1 pushing the result to 1.
    float3 denom = max(1.0 - fg, 1e-6);
    return saturate(bg / denom);
}

float3 blendColorBurn(float3 fg, float3 bg) {
    // 1 - (1-bg)/fg, with fg<=0 collapsing the result to 0.
    float3 denom = max(fg, 1e-6);
    return saturate(1.0 - (1.0 - bg) / denom);
}

float3 blendDarken(float3 fg, float3 bg)     { return min(fg, bg); }
float3 blendLighten(float3 fg, float3 bg)    { return max(fg, bg); }
float3 blendDifference(float3 fg, float3 bg) { return abs(fg - bg); }
float3 blendExclusion(float3 fg, float3 bg)  { return fg + bg - 2.0 * fg * bg; }

float4 PSMain(PSInput input) : SV_TARGET {
    // Sample fg via the interpolated UVs from the VS (clip-space quad UVs).
    float4 fg = fgTexture.Sample(samplerState, input.texCoord);

    // Sample bg from the snapshot at the screen-space pixel we're writing to.
    uint bgW, bgH;
    bgTexture.GetDimensions(bgW, bgH);
    float2 bgUV = input.position.xy / float2(bgW, bgH);
    float4 bg = bgTexture.Sample(samplerState, bgUV);

    // Compute the blended RGB. Defaults to fg if the mode isn't recognised
    // (defensive - shouldn't happen since drawTexturedQuad routes the
    // fixed-function modes elsewhere).
    float3 blended = fg.rgb;
    switch (blendMode) {
        case BLEND_MODE_OVERLAY:     blended = blendOverlay(fg.rgb, bg.rgb);     break;
        case BLEND_MODE_SOFT_LIGHT:  blended = blendSoftLight(fg.rgb, bg.rgb);   break;
        case BLEND_MODE_HARD_LIGHT:  blended = blendHardLight(fg.rgb, bg.rgb);   break;
        case BLEND_MODE_COLOR_DODGE: blended = blendColorDodge(fg.rgb, bg.rgb);  break;
        case BLEND_MODE_COLOR_BURN:  blended = blendColorBurn(fg.rgb, bg.rgb);   break;
        case BLEND_MODE_DARKEN:      blended = blendDarken(fg.rgb, bg.rgb);      break;
        case BLEND_MODE_LIGHTEN:     blended = blendLighten(fg.rgb, bg.rgb);     break;
        case BLEND_MODE_DIFFERENCE:  blended = blendDifference(fg.rgb, bg.rgb);  break;
        case BLEND_MODE_EXCLUSION:   blended = blendExclusion(fg.rgb, bg.rgb);   break;
    }

    // Composite over bg using the fg's effective alpha (Porter-Duff "over").
    // Where fg is transparent, the bg passes through unchanged.
    float a = saturate(fg.a * opacity);
    float3 outRGB = lerp(bg.rgb, blended, a);
    float outA = bg.a + a * (1.0 - bg.a);

    return float4(outRGB, outA);
}
