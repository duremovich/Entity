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
    uint blendMode;         // Blend mode: 0=Normal, 1=Add, 2=Multiply, 3=Screen
    float padding2;
    float padding3;
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
