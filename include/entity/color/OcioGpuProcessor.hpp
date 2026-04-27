#pragma once

#include <OpenColorIO/OpenColorIO.h>

#include <cstdint>
#include <string>
#include <vector>

namespace entity {

/**
 * OcioGpuProcessor — a self-contained value type holding everything a
 * pipeline needs to run an OCIO transform on the GPU:
 *
 *   - The HLSL function source emitted by OCIO's GpuShaderDesc.
 *   - The function's name (so callers know what to invoke).
 *   - The list of LUT textures the function expects bound (1D / 2D / 3D).
 *   - The list of uniforms the function expects (currently we expose the
 *     OCIO UniformData unmodified; subtask 5 is responsible for marshaling
 *     into a constant buffer).
 *
 * OcioManager builds these via buildInputProcessor / buildDisplayProcessor
 * and caches the result. Lifetimes:
 *
 *   - The HLSL `shaderText`, function name, texture binding metadata, and
 *     **copies of LUT data** are owned by this object and stable across
 *     OcioManager restarts.
 *   - The uniform getter callbacks reference state inside the underlying
 *     OCIO Processor; OcioGpuProcessor keeps a `ConstProcessorRcPtr` alive
 *     to ensure those references stay valid.
 *
 * Phase C.12 #4 only populates this struct and validates emission.
 * Subtask 5 wires LUT upload + descriptor heap binding through to the
 * actual rendering pipeline.
 */
class OcioGpuProcessor {
public:
    enum class TextureKind {
        Lut1D,    // 1D LUT (TEXTURE_1D, height=1)
        Lut2D,    // 2D LUT (TEXTURE_2D, height>1) — used for huge LUTs OCIO splits across rows
        Lut3D     // 3D LUT (edgelen × edgelen × edgelen, RGB)
    };

    struct TextureBinding {
        std::string textureName;     // Variable name in the generated HLSL
        std::string samplerName;     // Sampler name (mirrors textureName by convention)
        TextureKind kind{TextureKind::Lut1D};
        uint32_t    width{0};         // 1D: total samples; 2D: x-width; 3D: edgelen
        uint32_t    height{0};        // 1D: 1; 2D: rows; 3D: edgelen (also)
        uint32_t    depth{0};         // 3D only: edgelen
        bool        isRgb{true};      // false ⇒ single-channel red LUT (rare)
        OCIO_NAMESPACE::Interpolation interpolation{OCIO_NAMESPACE::INTERP_LINEAR};
        std::vector<float> values;    // RGB triplets for isRgb, single floats otherwise
    };

    struct UniformBinding {
        std::string name;
        OCIO_NAMESPACE::GpuShaderDesc::UniformData rawData{};
    };

    OcioGpuProcessor() = default;

    const std::string&                  functionName() const { return m_functionName; }
    const std::string&                  shaderText()   const { return m_shaderText; }
    const std::vector<TextureBinding>&  textures()     const { return m_textures; }
    const std::vector<UniformBinding>&  uniforms()     const { return m_uniforms; }

    bool empty() const { return m_shaderText.empty(); }

private:
    friend class OcioManager;

    OCIO_NAMESPACE::ConstProcessorRcPtr m_processor;   // keeps uniform getters alive
    std::string                  m_functionName;
    std::string                  m_shaderText;
    std::vector<TextureBinding>  m_textures;
    std::vector<UniformBinding>  m_uniforms;
};

} // namespace entity
