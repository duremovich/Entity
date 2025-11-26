/**
 * Composite Vertex Shader
 *
 * Transforms vertices for fullscreen quad rendering with layer transforms.
 * Supports 3D rotations (X/Y/Z) for 2D compositing by clamping Z to prevent clipping.
 */

#include "common.hlsli"

PSInput VSMain(VSInput input) {
    PSInput output;

    // Apply transformation matrix
    float4 worldPos = mul(float4(input.position, 1.0f), transform);

    // For 2D compositing with 3D rotations (X/Y axis), we need to prevent
    // vertices from being clipped by the near/far planes.
    // D3D clip space Z range is 0 to 1 (not -1 to 1 like OpenGL).
    // Clamp Z to a safe range within the valid clip volume.
    // This allows X/Y rotation for perspective-like effects without clipping.
    worldPos.z = clamp(worldPos.z, 0.01f, 0.99f);

    output.position = worldPos;

    // Pass through texture coordinates
    output.texCoord = input.texCoord;

    return output;
}
