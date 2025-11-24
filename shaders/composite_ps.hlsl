/**
 * Composite Pixel Shader
 *
 * Samples layer texture and applies opacity.
 * Assumes premultiplied alpha input for correct alpha blending.
 */

#include "common.hlsli"

// Texture and sampler
Texture2D layerTexture : register(t0);
SamplerState samplerState : register(s0);

float4 PSMain(PSInput input) : SV_TARGET {
    // Sample the texture
    float4 color = layerTexture.Sample(samplerState, input.texCoord);

    // Apply layer opacity
    // Since we're using premultiplied alpha, we multiply both RGB and A by opacity
    color *= opacity;

    // Return premultiplied alpha color
    // The blend state (SRC_ALPHA, INV_SRC_ALPHA) will handle compositing correctly
    return color;
}
