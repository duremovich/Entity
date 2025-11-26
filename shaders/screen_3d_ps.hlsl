/**
 * 3D Screen Pixel Shader
 *
 * Samples the composited texture and outputs it.
 * Simple passthrough with optional border highlight for selection.
 */

// Texture and sampler
Texture2D screenTexture : register(t0);
SamplerState screenSampler : register(s0);

// Constants
cbuffer ScreenPixelConstants : register(b1) {
    float4 borderColor;         // Color for screen border
    float borderWidth;          // Border width in UV space (0-1)
    float selected;             // 1.0 if selected, 0.0 otherwise
    float brightness;           // Brightness multiplier
    float padding;              // Alignment padding
};

// From vertex shader
struct PixelInput {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

float4 main(PixelInput input) : SV_TARGET {
    // Sample the texture
    float4 color = screenTexture.Sample(screenSampler, input.texCoord);

    // Apply brightness
    color.rgb *= brightness;

    // Draw border if selected
    if (selected > 0.5) {
        float2 edgeDist = min(input.texCoord, 1.0 - input.texCoord);
        float minDist = min(edgeDist.x, edgeDist.y);

        if (minDist < borderWidth) {
            // Blend border color
            float borderFactor = 1.0 - smoothstep(0.0, borderWidth, minDist);
            color = lerp(color, borderColor, borderFactor * 0.8);
        }
    }

    return color;
}
