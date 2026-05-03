// Mesh triangle vertex shader.
// Reads 3 NDC vertex positions + 3 UVs from root constants and emits them
// keyed by SV_VertexID. The standard GPU triangle rasterizer then does
// proper barycentric interpolation of UVs across the triangle interior —
// no bilinear-vs-barycentric distortion like drawMappingSurface has.

cbuffer MeshTriangleConstants : register(b0)
{
    float4 verts[3];  // .xy = NDC position, .zw = unused
    float4 uvs[3];    // .xy = UV, .zw = unused
};

void VSMain(uint vid : SV_VertexID,
            out float4 pos : SV_Position,
            out float2 uv  : TEXCOORD0)
{
    uint i = min(vid, 2u);
    pos = float4(verts[i].xy, 0.5, 1.0);
    uv  = uvs[i].xy;
}
