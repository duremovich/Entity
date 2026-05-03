// Calibration overlay vertex shader.
// Generates a fullscreen triangle from SV_VertexID — no vertex buffer needed.
// Pairs with calibration_overlay_ps.hlsl.

void VSMain(uint vid : SV_VertexID,
            out float4 pos : SV_Position,
            out float2 uv  : TEXCOORD0)
{
    // Vertices 0,1,2 → UVs (0,0), (2,0), (0,2)
    // The triangle covers [-1,1]² clip space completely.
    uv  = float2((vid == 1) ? 2.0 : 0.0,
                 (vid == 2) ? 2.0 : 0.0);
    pos = float4(uv.x * 2.0 - 1.0,
                 1.0 - uv.y * 2.0,
                 0.0, 1.0);
}
