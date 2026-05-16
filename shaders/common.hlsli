/**
 * Common HLSL structures and functions
 * Shared between vertex and pixel shaders
 */

// Vertex shader input
struct VSInput {
    float3 position : POSITION;
    float2 texCoord : TEXCOORD0;
};

// Vertex shader output / Pixel shader input
struct PSInput {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

// Constant buffer for per-layer properties (must match C++ LayerConstants struct)
cbuffer LayerConstants : register(b0) {
    float4x4 transform;     // Transformation matrix
    float4 color;           // Layer color (RGBA)
    float opacity;          // Layer opacity (0.0 - 1.0)
    uint blendMode;         // Blend mode: 0=Normal, 1=Add, 2=Multiply, 3=Screen, ...
    uint colorSpace;        // Texture colour space: 0=Linear, 1=YCoCg_scaled (HAP Q)
    float padding3;
    // Source-UV crop (ADR-0021 M3 Tiled mode). xy = uv offset, zw = uv size.
    // Identity (0,0,1,1) = full source frame. Applied in composite_vs as
    // `output.texCoord = input.texCoord * uvRect.zw + uvRect.xy`.
    float4 uvRect;
};

// Blend mode enumeration (must match C++ BlendMode enum)
#define BLEND_MODE_NORMAL       0
#define BLEND_MODE_ADD          1
#define BLEND_MODE_MULTIPLY     2
#define BLEND_MODE_SCREEN       3
#define BLEND_MODE_OVERLAY      4
#define BLEND_MODE_SOFT_LIGHT   5
#define BLEND_MODE_HARD_LIGHT   6
#define BLEND_MODE_COLOR_DODGE  7
#define BLEND_MODE_COLOR_BURN   8
#define BLEND_MODE_DARKEN       9
#define BLEND_MODE_LIGHTEN      10
#define BLEND_MODE_DIFFERENCE   11
#define BLEND_MODE_EXCLUSION    12

// Texture colour-space enumeration (must match C++ TextureColorSpace enum)
#define COLOR_SPACE_LINEAR        0
#define COLOR_SPACE_YCOCG_SCALED  1

// Decode HAP Q's scaled-YCoCg sample into linear RGB. The packing per the
// Vidvox HAP spec:
//   .a    = Y (BC3 alpha block, high precision)
//   .r    = Co + 0.5 (offset-binary)
//   .g    = Cg + 0.5
//   .b    = (scale - 1.0) / 31.875  -> stored 5-bit dynamic-range factor
// Scale-up is the inverse: scale = b * 255/8 + 1 ; chroma is divided by it.
// Final: rgb = matrixYCoCgToRGB(Y, Co, Cg). Output alpha is whatever the
// caller wants (1.0 if no separate alpha plane; otherwise sampled separately
// from a second BC4 texture for HapM — not yet wired here).
float4 decodeHapQ(float4 sampled) {
    const float4 offsets = float4(-0.50196078431373, -0.50196078431373, 0.0, 0.0);
    sampled += offsets;
    float scale = (sampled.b * (255.0 / 8.0)) + 1.0;
    float Co = sampled.r / scale;
    float Cg = sampled.g / scale;
    float Y  = sampled.a;
    return float4(Y + Co - Cg, Y + Cg, Y - Co - Cg, 1.0);
}

// Sample the layer texture and apply colour-space conversion if the cbuffer
// hint says so. Branches on a uniform; modern GPUs handle this in hardware
// dynamic branching with no per-pixel cost.
//
// Phase C.12 #5: when ENTITY_OCIO_INPUT_FN is defined (set via -D at runtime
// DXC compile by D3D12Renderer + OcioManager), the sampled-and-decoded
// pixel is fed through the OCIO-emitted input transform function. The
// function source is prepended to the shader by D3D12Renderer's splicer,
// so by the time HLSL compilation happens, ENTITY_OCIO_INPUT_FN refers to
// a real function (e.g. OCIO_input_Linear_Rec_709_sRGB_).
//
// HAP Q's YCoCg decode runs FIRST so OCIO sees linear Rec.709 RGB, the
// post-decode colour space the per-clip OCIO tag refers to.
//
// Offline DXC compilation (apps/CMakeLists.txt) leaves the macro undefined,
// so the offline .cso is identical to the pre-C.12 behaviour.
float4 sampleLayerTexture(Texture2D tex, SamplerState samp, float2 uv) {
    float4 raw = tex.Sample(samp, uv);
    if (colorSpace == COLOR_SPACE_YCOCG_SCALED) {
        raw = decodeHapQ(raw);
    }
#ifdef ENTITY_OCIO_INPUT_FN
    raw = ENTITY_OCIO_INPUT_FN(raw);
#endif
    return raw;
}
