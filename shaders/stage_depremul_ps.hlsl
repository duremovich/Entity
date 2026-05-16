/**
 * Stage de-premultiply pixel shader.
 *
 * The stage 3D mesh pass renders premultiplied RGBA into m_stageTarget
 * so multiple transparent meshes blend correctly within the RT via
 * premul-over. ImGui's downstream composite (the AddImage that paints
 * the stage RT onto the editor window) uses standard straight-alpha
 * blend math, so it expects straight RGB. This pass converts:
 *
 *     straight.rgb = premul.rgb / max(premul.a, eps)
 *
 * Caller (D3D12Renderer::applyStageDePremul) reads the premul stage RT
 * via t0 and writes to the sibling straight RT.
 */

Texture2D    g_tex : register(t0);
SamplerState g_smp : register(s0);

float4 PSMain(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    float4 c = g_tex.Sample(g_smp, uv);
    c.rgb /= max(c.a, 1e-4);
    return c;
}
