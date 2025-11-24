/**
 * Composite Vertex Shader
 *
 * Transforms vertices for fullscreen quad rendering with layer transforms.
 */

#include "common.hlsli"

PSInput VSMain(VSInput input) {
    PSInput output;

    // Apply transformation matrix
    float4 worldPos = mul(float4(input.position, 1.0f), transform);
    output.position = worldPos;

    // Pass through texture coordinates
    output.texCoord = input.texCoord;

    return output;
}
