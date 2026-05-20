// Calibration overlay pixel shader.
// Draws numbered crosshair markers at given UV positions on a black background.
// Active crosshair (activeIdx) is yellow; others are white.

cbuffer CalibrationCB : register(b0)
{
    // 16 crosshair positions packed into 8 float4s:
    //   pts_packed[i].xy = point 2i
    //   pts_packed[i].zw = point 2i+1
    float4 pts_packed[8]; // 8 * 16 = 128 bytes
    int    numPts;        // number of active points (0..16)
    int    activeIdx;     // index of the crosshair being positioned (-1 = none)
    int    checkerboard;  // 1 = full-frame quadrant pattern centered on activeIdx
    float  _pad;
};

float2 getPoint(int i)
{
    // Even index → .xy half; odd index → .zw half
    return (i & 1) == 0 ? pts_packed[i >> 1].xy
                        : pts_packed[i >> 1].zw;
}

float4 PSMain(float4 svPos : SV_Position,
              float2 uv    : TEXCOORD0) : SV_Target
{
    // Precision cursor: full-frame quadrant pattern centered on
    // the active crosshair. The user aligns the sharp 4-quadrant intersection
    // against a physical feature for sub-pixel cursor placement. White covers
    // the projector view entirely, so the user can see EVERY pixel of the
    // pattern on the physical surface.
    if (checkerboard != 0 && activeIdx >= 0 && activeIdx < numPts)
    {
        float2 c = getPoint(activeIdx);
        bool right = uv.x > c.x;
        bool below = uv.y > c.y;
        // Top-left + bottom-right = white; top-right + bottom-left = black.
        float v = (right == below) ? 1.0 : 0.0;
        // Thin gray reference lines through the cursor — helps locate the
        // intersection from a distance even when the four quadrants blur.
        float2 d = abs(uv - c);
        if (d.x < 0.0008 || d.y < 0.0008) v = 0.5;
        return float4(v, v, v, 1.0);
    }

    // Standard crosshair markers — small +'s at each correspondence point.
    const float ARM_LEN   = 0.030;
    const float ARM_WIDTH = 0.004;

    for (int i = 0; i < numPts; ++i)
    {
        float2 center = getPoint(i);
        float2 d = abs(uv - center);

        bool onH = (d.y < ARM_WIDTH) && (d.x < ARM_LEN);
        bool onV = (d.x < ARM_WIDTH) && (d.y < ARM_LEN);

        if (onH || onV)
        {
            float3 col = (i == activeIdx) ? float3(1, 1, 0) : float3(1, 1, 1);
            return float4(col, 1.0);
        }
    }

    // Transparent background — composited over the projector view in OutputManager.
    return float4(0, 0, 0, 0);
}
