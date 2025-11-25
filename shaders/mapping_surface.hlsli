/**
 * Mapping Surface Common Structures
 * Shared between perspective warp vertex and pixel shaders
 */

// Vertex shader input (quad corner index)
struct MappingVSInput {
    float3 position : POSITION;
    float2 texCoord : TEXCOORD0;
};

// Vertex shader output / Pixel shader input
struct MappingPSInput {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
    float2 surfaceUV : TEXCOORD1;  // UV within surface for soft edge calc
};

// Constant buffer for mapping surface properties
cbuffer MappingSurfaceConstants : register(b0) {
    // Corner positions in clip space (-1 to 1)
    // Order: TL, TR, BR, BL
    float4 corners[4];

    // Source UV coordinates for each corner
    // Order: TL, TR, BR, BL
    float4 sourceUVs[4];

    // Soft edge amounts (0-1 range, percentage of surface)
    float softEdgeLeft;
    float softEdgeRight;
    float softEdgeTop;
    float softEdgeBottom;

    // Surface properties
    float brightness;
    float gamma;
    float opacity;
    float padding1;
};

/**
 * Compute soft edge multiplier based on position within surface.
 * Returns a value between 0 and 1 for edge blending.
 */
float computeSoftEdge(float2 uv) {
    float edge = 1.0f;

    // Left edge fade
    if (softEdgeLeft > 0.0f && uv.x < softEdgeLeft) {
        edge *= smoothstep(0.0f, softEdgeLeft, uv.x);
    }

    // Right edge fade
    if (softEdgeRight > 0.0f && uv.x > (1.0f - softEdgeRight)) {
        edge *= smoothstep(1.0f, 1.0f - softEdgeRight, uv.x);
    }

    // Top edge fade
    if (softEdgeTop > 0.0f && uv.y < softEdgeTop) {
        edge *= smoothstep(0.0f, softEdgeTop, uv.y);
    }

    // Bottom edge fade
    if (softEdgeBottom > 0.0f && uv.y > (1.0f - softEdgeBottom)) {
        edge *= smoothstep(1.0f, 1.0f - softEdgeBottom, uv.y);
    }

    return edge;
}

/**
 * Apply gamma correction.
 */
float3 applyGamma(float3 color, float gammaValue) {
    return pow(abs(color), 1.0f / gammaValue);
}
