// Shape generator — a single centred shape (circle / rectangle / ring)
// in a flat color over transparent black. Never samples g_input (zero
// texture-input sockets).
//
// Param schema:
//   [0].x    = shape    enum: 0 = circle, 1 = rectangle, 2 = ring
//   [1].x    = size     [0, 1] half-extent relative to the frame
//   [2].x    = softness [0, 0.5] edge feather width
//   [3].xyzw = color    (linear RGBA)

#include "_effect_common.hlsli"

float4 PSMain(EffectVSOut i) : SV_TARGET {
    int   shape    = asint(g_params[0].x);  // Enum slots are bit-cast ints
    float size     = saturate(g_params[1].x);
    float softness = clamp(g_params[2].x, 0.0f, 0.5f);
    float4 color   = g_params[3];

    // Aspect-corrected centered coords so circles stay round.
    float2 d = i.uv - 0.5f;
    float aspect = (g_viewportSize.y > 0.0f)
        ? g_viewportSize.x / g_viewportSize.y : 1.0f;
    d.x *= aspect;

    float mask = 0.0f;
    if (shape == 1) {
        // Rectangle: signed distance to an axis-aligned box.
        float2 q = abs(i.uv - 0.5f) - size * 0.5f;
        float sd = max(q.x, q.y);
        mask = 1.0f - smoothstep(0.0f, softness + 1e-4f, sd);
    } else if (shape == 2) {
        // Ring: band around radius `size * 0.5`.
        float r = length(d);
        float band = softness + 0.02f;
        mask = 1.0f - smoothstep(band, band + softness + 1e-4f,
                                 abs(r - size * 0.5f));
    } else {
        // Circle.
        float r = length(d);
        mask = 1.0f - smoothstep(size * 0.5f,
                                 size * 0.5f + softness + 1e-4f, r);
    }

    return float4(color.rgb, color.a) * mask;
}
