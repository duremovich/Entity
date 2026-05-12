// Common cbuffer + bindings for all per-layer effects (PASS 1.5 of the
// compositor, issue #54).
//
// All effect shaders share a single fullscreen-triangle VS (effect_vs.cso)
// and a single root signature: SRV table t0 (input texture) + CBV b0
// (param blob) + static sampler s0. Each effect provides its own PS.
//
// The cbuffer layout is FIXED at 256 bytes — exactly one D3D12 cbuffer
// alignment unit. The first 14 vec4 slots are param storage: each
// ParamSchema entry maps to one g_params[N] slot regardless of type
// (Float → .x, Vec2 → .xy, Color → .xyzw, Int → asint(.x), …). Effects
// reference parameters positionally by their schema index. Then a misc
// region: viewport size (used by blur and edge detection effects) plus
// padding to round out the 256 bytes.
//
// Editor-side bake (PlaybackTimeAuthority + EffectKindRegistry) marshals
// EffectParameters.values + EffectAnimatedParameters keyframe-evaluation
// results into this layout and ships the bytes through
// bus::EffectSnapshot::paramBlob. The renderer copies them verbatim into
// a per-frame ring buffer slot bound at root parameter 2 (b0).

#ifndef ENTITY_EFFECT_COMMON_HLSLI
#define ENTITY_EFFECT_COMMON_HLSLI

cbuffer EffectParams : register(b0) {
    float4 g_params[14];   //   0–223 : per-effect param slots
    float2 g_viewportSize; // 224–231 : input texture pixel dimensions
    float2 _ec_pad0;       // 232–239
    float4 _ec_pad1;       // 240–255 — total 256 bytes
};

Texture2D    g_input  : register(t0);
SamplerState g_samp   : register(s0);

float4 sampleInput(float2 uv) { return g_input.Sample(g_samp, uv); }

struct EffectVSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

#endif // ENTITY_EFFECT_COMMON_HLSLI
