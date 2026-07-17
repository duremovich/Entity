// Mask combiner — multiplies input A's alpha (and optionally color) by a
// channel of input B. The classic matte node: route a Shape or Noise
// generator into `mask` to cut a window out of any content.
// Sockets: a = t0, mask = t1.
//
// Param schema:
//   [0].x = channel  enum: 0 Luma, 1 Alpha, 2 R, 3 G, 4 B
//   [1].x = invert   bool (0 / 1)
//   [2].x = softness [0, 1] remaps the mask through a widening smoothstep

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    int   channel  = asint(g_params[0].x);       // Enum slots are bit-cast ints
    bool  invert   = asint(g_params[1].x) != 0;
    float softness = saturate(g_params[2].x);

    float4 a = sampleInput(i.uv);
    float4 m = sampleInput1(i.uv);

    float mask;
    switch (channel) {
        case 1:  mask = m.a; break;
        case 2:  mask = m.r; break;
        case 3:  mask = m.g; break;
        case 4:  mask = m.b; break;
        default: mask = dot(m.rgb, float3(0.2126f, 0.7152f, 0.0722f)); break;
    }
    if (invert) mask = 1.0f - mask;
    // Softness widens the transition band around 0.5.
    float lo = 0.5f - 0.5f * max(softness, 1e-4f);
    float hi = 0.5f + 0.5f * max(softness, 1e-4f);
    mask = smoothstep(lo, hi, mask);

    return float4(a.rgb * mask, a.a * mask);
}
