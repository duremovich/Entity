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
    float2 _pad;
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
    // Crosshair geometry in UV space
    const float ARM_LEN   = 0.030; // half-length of each arm
    const float ARM_WIDTH = 0.004; // half-width of each arm
    const float TICK_LEN  = 0.008; // centre tick (makes it look like a +)

    for (int i = 0; i < numPts; ++i)
    {
        float2 center = getPoint(i);
        float2 d = abs(uv - center);

        bool onH = (d.y < ARM_WIDTH) && (d.x < ARM_LEN);
        bool onV = (d.x < ARM_WIDTH) && (d.y < ARM_LEN);

        if (onH || onV)
        {
            // Active crosshair = yellow, locked points = white
            float3 col = (i == activeIdx) ? float3(1, 1, 0) : float3(1, 1, 1);
            return float4(col, 1.0);
        }
    }

    // Transparent background — composited over the projector view in OutputManager.
    return float4(0, 0, 0, 0);
}
