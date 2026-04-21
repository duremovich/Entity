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

    // Position: bilinear blend of the four corner positions.
    // corners: [0]=TL, [1]=TR, [2]=BR, [3]=BL
    float2 topEdge    = lerp(corners[0].xy, corners[1].xy, u);
    float2 bottomEdge = lerp(corners[3].xy, corners[2].xy, u);
    float2 position   = lerp(topEdge, bottomEdge, v);
    output.position = float4(position, 0.0f, 1.0f);

    // Perspective-correct UV via the q-coordinate trick.
    // Each corner carries (u*q, v*q, q). Bilinear interpolation across the
    // unit quad gives the correct values at the four corners; the rasterizer
    // then interpolates linearly across each triangle, and the pixel shader
    // recovers projective UV by dividing xy/z. Mathematically equivalent to
    // a full homography for convex quads, but with zero matrix math per
    // fragment.
    //
    // For a rectangle, sourceUVs[i].w is 2 at every corner and the divide
    // collapses to identity — no cost, no regression for the trivial case.
    float3 tl = float3(sourceUVs[0].xy * sourceUVs[0].w, sourceUVs[0].w);
    float3 tr = float3(sourceUVs[1].xy * sourceUVs[1].w, sourceUVs[1].w);
    float3 br = float3(sourceUVs[2].xy * sourceUVs[2].w, sourceUVs[2].w);
    float3 bl = float3(sourceUVs[3].xy * sourceUVs[3].w, sourceUVs[3].w);
    float3 topUVQ = lerp(tl, tr, u);
    float3 botUVQ = lerp(bl, br, u);
    output.perspUV = lerp(topUVQ, botUVQ, v);

    // Pass surface UV for soft edge calculation (stays in surface space,
    // unchanged by the projective warp — edge fade is defined at the
    // physical edge of the quad, not the sampled-texture edge).
    output.surfaceUV = float2(u, v);

    return output;
}
