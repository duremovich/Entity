/**
 * Floor Grid Pixel Shader
 *
 * Draws a procedural grid pattern on the floor with:
 * - Major and minor grid lines
 * - Distance-based fade for anti-aliasing
 * - Customizable colors
 */

// Constant buffer with grid parameters
cbuffer GridConstants : register(b1) {
    float4 majorLineColor;      // Color for major grid lines (every 1 unit)
    float4 minorLineColor;      // Color for minor grid lines
    float4 backgroundColor;     // Floor background color
    float majorGridSpacing;     // Distance between major lines (e.g., 1.0)
    float minorGridDivisions;   // Number of minor divisions per major cell
    float lineWidth;            // Base line width
    float fadeDistance;         // Distance at which grid starts to fade
};

// From vertex shader
struct PixelInput {
    float4 position : SV_POSITION;
    float2 worldXZ : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

// Calculate grid line intensity using screen-space derivatives
float gridLine(float coord, float lineWidth) {
    float derivative = fwidth(coord);
    float halfWidth = lineWidth * 0.5;
    return 1.0 - smoothstep(halfWidth - derivative, halfWidth + derivative, abs(frac(coord - 0.5) - 0.5));
}

float4 main(PixelInput input) : SV_TARGET {
    // Calculate distance from camera for fade effect
    float dist = length(input.worldPos);
    float fadeFactor = saturate(1.0 - (dist - fadeDistance) / fadeDistance);

    // Calculate grid coordinates
    float2 majorCoord = input.worldXZ / majorGridSpacing;
    float2 minorCoord = input.worldXZ / (majorGridSpacing / minorGridDivisions);

    // Calculate line intensities
    float majorIntensityX = gridLine(majorCoord.x, lineWidth);
    float majorIntensityY = gridLine(majorCoord.y, lineWidth);
    float majorIntensity = max(majorIntensityX, majorIntensityY);

    float minorIntensityX = gridLine(minorCoord.x, lineWidth * 0.5);
    float minorIntensityY = gridLine(minorCoord.y, lineWidth * 0.5);
    float minorIntensity = max(minorIntensityX, minorIntensityY);

    // Combine colors
    float4 color = backgroundColor;
    color = lerp(color, minorLineColor, minorIntensity * 0.3);
    color = lerp(color, majorLineColor, majorIntensity);

    // Apply distance fade
    color.a *= fadeFactor;

    // Add slight highlight at origin axes
    float originLineWidth = lineWidth * 2.0;
    float xAxisIntensity = gridLine(input.worldXZ.y / majorGridSpacing, originLineWidth);
    float zAxisIntensity = gridLine(input.worldXZ.x / majorGridSpacing, originLineWidth);

    // Red for X axis, Blue for Z axis (at origin)
    if (abs(input.worldXZ.y) < majorGridSpacing * 0.1) {
        color = lerp(color, float4(0.8, 0.2, 0.2, 1.0), xAxisIntensity * 0.5);
    }
    if (abs(input.worldXZ.x) < majorGridSpacing * 0.1) {
        color = lerp(color, float4(0.2, 0.2, 0.8, 1.0), zAxisIntensity * 0.5);
    }

    return color;
}
