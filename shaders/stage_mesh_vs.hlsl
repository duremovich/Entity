cbuffer FrameCB : register(b0) { float4x4 viewProj; };
cbuffer DrawCB  : register(b1) {
    float4x4 model;
    float4   tint;
    uint     renderMode;
    uint3    _pad;
};

struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; float3 nrm : NORMAL; };
struct VSOut { float4 sv : SV_POSITION; float2 uv : TEXCOORD0; float3 wn : NORMAL; };

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wp = mul(model, float4(v.pos, 1.0));
    o.sv = mul(viewProj, wp);
    o.uv = v.uv;
    o.wn = mul((float3x3)model, v.nrm);
    return o;
}
