/**
 * Composite Pixel Shader
 *
 * Samples layer texture and applies opacity.
 * Uses straight (non-premultiplied) alpha from FFmpeg video decode.
 */

#include "common.hlsli"

// Texture and sampler
Texture2D layerTexture : register(t0);
SamplerState samplerState : register(s0);

float4 PSMain(PSInput input) : SV_TARGET {
    // Sample the texture (straight alpha)
    float4 color = layerTexture.Sample(samplerState, input.texCoord);

    // Apply layer opacity to alpha channel only
    // RGB stays unchanged - blend state will handle multiplication
    color.a *= opacity;

    // Return straight alpha color
    // Blend state (SRC_ALPHA, INV_SRC_ALPHA) handles compositing
    return color;
}
