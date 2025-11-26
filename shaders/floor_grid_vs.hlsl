/**
 * Floor Grid Vertex Shader
 *
 * Transforms a floor grid plane from world space to clip space
 * using view-projection matrix from camera.
 */

// Constant buffer with camera matrices
cbuffer CameraConstants : register(b0) {
    float4x4 viewProjection;    // Combined view-projection matrix
    float3 cameraPosition;      // Camera world position (for distance fade)
    float gridSize;             // Size of grid cells
};

// Vertex input
struct VertexInput {
    float3 position : POSITION;
    float2 texCoord : TEXCOORD0;
};

// Vertex output
struct VertexOutput {
    float4 position : SV_POSITION;
    float2 worldXZ : TEXCOORD0;      // World position for grid calculation
    float3 worldPos : TEXCOORD1;     // Full world position for distance fade
};

VertexOutput main(VertexInput input) {
    VertexOutput output;

    // Transform to clip space
    float4 worldPos = float4(input.position, 1.0);
    output.position = mul(viewProjection, worldPos);

    // Pass world XZ coordinates for grid pattern
    output.worldXZ = worldPos.xz;
    output.worldPos = worldPos.xyz;

    return output;
}
