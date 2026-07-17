// Linear Gradient generator — replaces the layer's content with a
// two-color ramp. Never samples g_input (zero texture-input sockets).
//
// Param schema:
//   [0].x    = angle (degrees, 0 = left-to-right, CCW positive)
//   [1].xyzw = colorA (linear RGBA, at the ramp start)
//   [2].xyzw = colorB (linear RGBA, at the ramp end)

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float angle = radians(g_params[0].x);
    float4 colorA = g_params[1];
    float4 colorB = g_params[2];

    // Project the (centered) UV onto the gradient axis, normalised so the
    // ramp spans the full frame at every angle.
    float2 dir = float2(cos(angle), sin(angle));
    float  t   = dot(i.uv - 0.5f, dir);
    float  span = 0.5f * (abs(dir.x) + abs(dir.y));
    t = saturate(t / max(span * 2.0f, 1e-5f) + 0.5f);

    return lerp(colorA, colorB, t);
}
