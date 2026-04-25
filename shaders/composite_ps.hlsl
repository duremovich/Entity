/**
 * Composite Pixel Shader
 *
 * Samples layer texture and applies opacity.
 * Uses straight (non-premultiplied) alpha from FFmpeg video decode.
 *
 * For non-Normal blend modes, we premultiply RGB by alpha in the shader
 * because D3D12 blend state can't express (SrcAlpha * SrcColor * DestColor).
 */

#include "common.hlsli"

// Texture and sampler
Texture2D layerTexture : register(t0);
SamplerState samplerState : register(s0);

float4 PSMain(PSInput input) : SV_TARGET {
    // Sample the texture (straight alpha). For HAP Q the helper undoes the
    // scaled-YCoCg encoding before composition; for everything else it's a
    // plain Sample() and the dynamic-uniform branch costs ~nothing.
    float4 color = sampleLayerTexture(layerTexture, samplerState, input.texCoord);

    // Apply layer opacity to alpha channel
    color.a *= opacity;

    // For non-Normal blend modes, premultiply RGB by alpha
    // This is required because D3D12 blend state can't express SrcAlpha * SrcColor * DestColor
    // Normal blend: Src * SrcAlpha + Dest * (1 - SrcAlpha) - works with straight alpha
    // Multiply blend: Src * Dest * SrcAlpha + Dest * (1 - SrcAlpha) - needs premultiplied
    // Add blend: Src * SrcAlpha + Dest - needs premultiplied
    // Screen blend: similar to add
    if (blendMode != BLEND_MODE_NORMAL) {
        color.rgb *= color.a;
    }

    return color;
}
