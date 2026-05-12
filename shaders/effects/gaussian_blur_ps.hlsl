// Gaussian blur — single-pass approximation using 13 weighted taps
// distributed across both axes around the centre. Not separable (Phase
// 3 should swap in a proper two-pass horizontal/vertical separable
// blur when performance matters); good enough as a v1 effect to prove
// the pipeline.
//
// Param schema:
//   [0].x = radius   pixels of standard deviation, clamped at the
//                    runtime to avoid degenerate huge-radius blowups
//                    (>64 px is wasted on a 13-tap kernel anyway).

#include "_effect_common.hlsli"

static const int   kTapCount = 13;

// Pre-computed normalized Gaussian weights for sigma ≈ 1.0 sampling
// pattern; runtime radius scales the tap offsets.
static const float kWeights[kTapCount] = {
    0.018f, 0.034f, 0.060f, 0.092f, 0.121f, 0.137f, 0.137f,
    0.137f, 0.121f, 0.092f, 0.060f, 0.034f, 0.018f
};

// Offsets relative to centre; symmetric about 0.
static const float kOffsets[kTapCount] = {
    -3.0f, -2.5f, -2.0f, -1.5f, -1.0f, -0.5f,  0.0f,
     0.5f,  1.0f,  1.5f,  2.0f,  2.5f,  3.0f
};

float4 PSMain(EffectVSOut i) : SV_TARGET {
    float radius = max(g_params[0].x, 0.0f);

    if (radius < 0.001f) {
        return sampleInput(i.uv);
    }

    // Texel size in UV space — viewport size travels through the cbuffer
    // because Texture2D dimensions aren't a single intrinsic on SM 6.0.
    float2 texel = (g_viewportSize.x > 0.0f && g_viewportSize.y > 0.0f)
                       ? (1.0f / g_viewportSize)
                       : float2(1.0f / 1920.0f, 1.0f / 1080.0f);

    // Sample in a 2D "cross" pattern: kTapCount taps along x AND
    // kTapCount taps along y, summed (weights renormalize by 2x). Cheap;
    // visually close to a true 2D Gaussian at small radii.
    float4 acc = float4(0, 0, 0, 0);
    float  wSum = 0.0f;
    [unroll] for (int k = 0; k < kTapCount; ++k) {
        float2 uvX = i.uv + float2(kOffsets[k] * radius * texel.x, 0.0f);
        float2 uvY = i.uv + float2(0.0f, kOffsets[k] * radius * texel.y);
        acc += sampleInput(uvX) * kWeights[k];
        acc += sampleInput(uvY) * kWeights[k];
        wSum += 2.0f * kWeights[k];
    }
    return acc / max(wSum, 0.0001f);
}
