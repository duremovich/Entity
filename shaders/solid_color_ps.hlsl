/**
 * Solid Color Pixel Shader
 *
 * Renders a solid color quad with opacity.
 * Used for Phase 1 colored quad rendering before texture support.
 */

#include "common.hlsli"

float4 PSMain(PSInput input) : SV_TARGET {
    // Return the color from constant buffer with opacity applied
    float4 finalColor = color;
    finalColor.a *= opacity;

    return finalColor;
}
