/**
 * Mapping Surface Vertex Shader
 *
 * Performs corner-pin warping by interpolating vertex positions
 * between the four corner positions defined in the constant buffer.
 *
 * Input vertices should be a unit quad (0,0) to (1,1) with 6 vertices:
 * - Triangle 1: (0,0), (1,0), (1,1)
 * - Triangle 2: (0,0), (1,1), (0,1)
 *
 * The shader maps these to the arbitrary quad corners using bilinear interpolation.
 */

#include "mapping_surface.hlsli"

MappingPSInput VSMain(MappingVSInput input) {
    MappingPSInput output;

    // The input position.xy contains normalized coordinates (0-1)
    // representing position within the unit quad
    float u = input.texCoord.x;
    float v = input.texCoord.y;

    // Bilinear interpolation between corners
    // corners: [0]=TL, [1]=TR, [2]=BR, [3]=BL
    // Note: We use 1-v because screen Y is flipped (top=0 in UV, but corners[0] is top-left)

    // Interpolate top edge (TL to TR)
    float2 topEdge = lerp(corners[0].xy, corners[1].xy, u);

    // Interpolate bottom edge (BL to BR)
    float2 bottomEdge = lerp(corners[3].xy, corners[2].xy, u);

    // Interpolate between top and bottom (using v for vertical position)
    float2 position = lerp(topEdge, bottomEdge, v);

    // Output position in clip space
    output.position = float4(position, 0.0f, 1.0f);

    // Bilinear interpolation of source UVs
    // sourceUVs: [0]=TL, [1]=TR, [2]=BR, [3]=BL
    float2 topUV = lerp(sourceUVs[0].xy, sourceUVs[1].xy, u);
    float2 bottomUV = lerp(sourceUVs[3].xy, sourceUVs[2].xy, u);
    output.texCoord = lerp(topUV, bottomUV, v);

    // Pass surface UV for soft edge calculation
    output.surfaceUV = float2(u, v);

    return output;
}
