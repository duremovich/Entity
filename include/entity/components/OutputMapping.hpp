#pragma once

#include <string>
#include <glm/glm.hpp>

#ifdef _WIN32
#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
using Microsoft::WRL::ComPtr;
#endif

namespace entity {

/**
 * OutputMapping component for display output configuration.
 *
 * Maps rendered content to specific physical displays using
 * EDID-based persistent identification.
 */
struct OutputMapping {
    uint32_t displayIndex{0};           // Index in enumerated displays
    std::string displayEDID;            // EDID identifier (persistent)
    std::string displayName;            // Human-readable name

    glm::ivec2 position{0, 0};          // Display position in desktop coordinates
    glm::ivec2 resolution{1920, 1080};  // Display resolution

#ifdef _WIN32
    ComPtr<IDXGIOutput> output;         // DXGI output interface
    ComPtr<IDXGISwapChain3> swapChain;  // Swap chain for this display
    ComPtr<ID3D12Resource> renderTarget;  // Render target texture
#endif

    bool isPrimary{false};              // True if primary display
    bool isConnected{true};             // True if display is connected

    /**
     * Check if output is valid and ready for rendering.
     */
    bool isValid() const {
#ifdef _WIN32
        return swapChain.Get() != nullptr && isConnected;
#else
        return false;
#endif
    }

    /**
     * Get aspect ratio of the display.
     */
    float getAspectRatio() const {
        if (resolution.y == 0) return 16.0f / 9.0f;  // Default to 16:9
        return static_cast<float>(resolution.x) / static_cast<float>(resolution.y);
    }
};

} // namespace entity
