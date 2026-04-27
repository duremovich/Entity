/**
 * Full-screen-triangle vertex shader for the C.12 #3 capture / tone-mapping
 * pass. Pulls UV + clip-space position from SV_VertexID — no vertex buffer
 * binding required.
 *
 * The triangle covers the (-1..1, -1..1) clip-space rectangle with one extra
 * corner; the rasterizer clips off the half outside the viewport. Standard
 * AMD/NVIDIA recipe; same vertex layout used by Disguise / Resolve.
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
