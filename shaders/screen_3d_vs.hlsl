/**
 * 3D Screen Vertex Shader
 *
 * Renders a textured quad (the composited video output) in 3D space.
 * The screen is positioned at the world origin, centered and facing +Z.
 */

// Constant buffer with camera and screen matrices
cbuffer ScreenConstants : register(b0) {
    float4x4 viewProjection;    // Combined view-projection matrix
    float4x4 worldMatrix;       // Screen's world transform (position, rotation, scale)
};

// Vertex input
struct VertexInput {
    float3 position : POSITION;
    float2 texCoord : TEXCOORD0;
};

// Vertex output
struct VertexOutput {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

VertexOutput main(VertexInput input) {
    VertexOutput output;

    // Transform through world matrix then view-projection
    float4 worldPos = mul(worldMatrix, float4(input.position, 1.0));
    output.position = mul(viewProjection, worldPos);

    // Pass texture coordinates
    output.texCoord = input.texCoord;

    return output;
}
