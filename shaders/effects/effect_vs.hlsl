// Fullscreen-triangle vertex shader shared by every effect PS in PASS 1.5.
// Three vertex IDs trace one triangle that covers the entire viewport,
// producing UV coordinates in [0, 1] with the standard "Y=0 at top" image
// convention.

#include "_effect_common.hlsli"

EffectVSOut VSMain(uint vid : SV_VertexID) {
    EffectVSOut o;
    // Standard fullscreen-triangle trick: UVs (0,0), (2,0), (0,2) map to
    // NDC (-1,1), (3,1), (-1,-3). The triangle is clipped to the viewport
    // and produces UVs in [0,1].
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv  = uv;
    o.pos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
    // Flip Y so uv (0,0) corresponds to the top of the image.
    o.pos.y = -o.pos.y;
    return o;
}
