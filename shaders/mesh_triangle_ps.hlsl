// Mesh triangle pixel shader.
// Samples the source texture using the rasterizer-interpolated UV (which is
// barycentric across the triangle — proper texture mapping, no bilinear
// approximation artifacts).

Texture2D    g_tex : register(t0);
SamplerState g_samp : register(s0);

float4 PSMain(float4 pos : SV_Position,
              float2 uv  : TEXCOORD0) : SV_Target
{
    return g_tex.Sample(g_samp, uv);
}
