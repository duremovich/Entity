/**
 * Fullscreen-triangle vertex shader for the stage de-premultiply pass.
 *
 * Mirrors aces_capture_vs.hlsl — synthesises a fullscreen triangle from
 * SV_VertexID, no vertex buffer needed. The triangle covers the (-1..1)
 * clip-space rect with one corner outside; the rasterizer clips it.
 *
 * Pairs with stage_depremul_ps.hlsl, called from
 * D3D12Renderer::applyStageDePremul() after the stage 3D mesh pass.
 */

struct VSOutput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint vid : SV_VertexID) {
    VSOutput o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
