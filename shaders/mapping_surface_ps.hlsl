/**
 * Mapping Surface Pixel Shader
 *
 * Samples the video texture and applies:
 * - Soft edge blending for multi-projector setups
 * - Brightness adjustment
 * - Gamma correction
 * - Opacity
 *
 * Uses straight (non-premultiplied) alpha from FFmpeg video decode.
 */

#include "mapping_surface.hlsli"

// Texture and sampler
Texture2D videoTexture : register(t0);
SamplerState textureSampler : register(s0);

float4 PSMain(MappingPSInput input) : SV_TARGET {
    // Projective UV recovery: (u*q, v*q) / q. See mapping_surface.hlsli for
    // the q-trick explanation. For a rectangle, q is constant and this is a
    // no-op; for a warped quad, this eliminates the diagonal crease.
    float2 uv = input.perspUV.xy / input.perspUV.z;

    // Sample the video texture (straight alpha)
    float4 color = videoTexture.Sample(textureSampler, uv);

    // Apply brightness
    color.rgb *= brightness;

    // Phase C.12 #5: when ENTITY_OCIO_DISPLAY_FN is defined (set via -D at
    // runtime DXC compile by D3D12Renderer + OcioManager), the linear ACEScg
    // working-space colour goes through the OCIO display+view transform —
    // this is what writes the actual sRGB / Rec.709 / etc. bytes to the
    // swap chain. The OCIO function source is prepended to the shader by
    // the splicer so by HLSL compile time ENTITY_OCIO_DISPLAY_FN names a
    // real function (e.g. OCIO_display_sRGB__Display_ACES_1_0__SDR_Video).
    //
    // gamma is now a post-ODT calibration trim, not the primary EOTF.
    //
    // Offline DXC compilation leaves the macro undefined and the path stays
    // pre-C.12 — applyGamma alone, no OCIO. Used as a fallback PSO when
    // OcioManager hasn't been set on the renderer.
#ifdef ENTITY_OCIO_DISPLAY_FN
    color.rgb = ENTITY_OCIO_DISPLAY_FN(float4(color.rgb, 1.0)).rgb;
    if (gamma != 1.0f) {
        color.rgb = applyGamma(color.rgb, gamma);
    }
#else
    if (gamma != 1.0f) {
        color.rgb = applyGamma(color.rgb, gamma);
    }
#endif

    // Compute soft edge multiplier
    float edgeFade = computeSoftEdge(input.surfaceUV);

    // Apply soft edge and opacity to alpha channel only
    // RGB stays unchanged - blend state handles multiplication by alpha
    color.a *= opacity * edgeFade;

    return color;
}
