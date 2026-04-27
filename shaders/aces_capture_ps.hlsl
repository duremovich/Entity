/**
 * Capture / tone-mapping pixel shader (Phase C.12 #3 — sRGB stub).
 *
 * Samples the FP16 linear compose target and writes UNORM8 sRGB-encoded
 * bytes for the integration-test golden readback path. The stub here does
 * the textbook linear→sRGB transfer function only; subtask 5 swaps this
 * for an OCIO-emitted display transform (sRGB display + view from the
 * loaded OCIO config) so the captured PNG matches what mapping_surface_ps
 * actually puts on a sRGB monitor.
 *
 * During the CI-red window (subtasks 3-5) the goldens hash output that
 * doesn't match production rendering yet — that's expected; subtask 6
 * rebakes the 22 golden hashes once input transforms also land.
 */

Texture2D<float4>  srcTexture : register(t0);
SamplerState       srcSampler : register(s0);

struct PSInput {
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

float3 linearToSrgb(float3 c) {
    c = max(c, 0.0);
    float3 lo = 12.92 * c;
    float3 hi = 1.055 * pow(c, 1.0 / 2.4) - 0.055;
    // HLSL 2021 doesn't permit short-circuiting ternaries on vectors —
    // `select` performs per-component branching the way we want.
    return select(c <= 0.0031308, lo, hi);
}

float4 PSMain(PSInput i) : SV_TARGET {
    float4 src = srcTexture.Sample(srcSampler, i.uv);
    return float4(linearToSrgb(src.rgb), src.a);
}
