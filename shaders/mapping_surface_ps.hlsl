/**
 * Mapping Surface Pixel Shader
 *
 * Samples the video texture and applies:
 * - Soft edge blending for multi-projector setups
 * - Brightness adjustment
 * - Gamma correction
 * - Opacity
 *
 * Uses straight (non-premultiplied) alpha from FFmpeg video decode.
 */

#include "mapping_surface.hlsli"

// Texture and sampler
Texture2D videoTexture : register(t0);
SamplerState textureSampler : register(s0);

float4 PSMain(MappingPSInput input) : SV_TARGET {
    // Sample the video texture (straight alpha)
    float4 color = videoTexture.Sample(textureSampler, input.texCoord);

    // Apply brightness
    color.rgb *= brightness;

    // Apply gamma correction
    if (gamma != 1.0f) {
        color.rgb = applyGamma(color.rgb, gamma);
    }

    // Compute soft edge multiplier
    float edgeFade = computeSoftEdge(input.surfaceUV);

    // Apply soft edge and opacity to alpha channel only
    // RGB stays unchanged - blend state handles multiplication by alpha
    color.a *= opacity * edgeFade;

    return color;
}
