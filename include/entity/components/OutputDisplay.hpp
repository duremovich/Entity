#pragma once

#include "../core/Types.hpp"
#include <string>
#include <cstdint>
#include <entt/entt.hpp>

namespace entity {

/**
 * Type of output display.
 */
enum class OutputType {
    Physical,    // Physical display (monitor, projector)
    Preview,     // Preview window within the application
    NDI,         // NDI network output
    Virtual      // Virtual output for testing/visualization
};

/**
 * Input source region definition.
 * Defines which region of the composited raster feeds this output.
 */
struct InputRegion {
    // Region in normalized coordinates (0-1)
    float x{0.0f};       // Left edge
    float y{0.0f};       // Top edge
    float width{1.0f};   // Width (1.0 = full width)
    float height{1.0f};  // Height (1.0 = full height)

    // Pixel coordinates (computed from normalized coords and raster size)
    int32_t pixelX{0};
    int32_t pixelY{0};
    int32_t pixelWidth{1920};
    int32_t pixelHeight{1080};

    /**
     * Update pixel coordinates from normalized coords and raster size.
     */
    void updatePixelCoords(int32_t rasterWidth, int32_t rasterHeight) {
        pixelX = static_cast<int32_t>(x * rasterWidth);
        pixelY = static_cast<int32_t>(y * rasterHeight);
        pixelWidth = static_cast<int32_t>(width * rasterWidth);
        pixelHeight = static_cast<int32_t>(height * rasterHeight);
    }

    /**
     * Set region from pixel coordinates.
     */
    void setFromPixels(int32_t px, int32_t py, int32_t pw, int32_t ph,
                       int32_t rasterWidth, int32_t rasterHeight) {
        pixelX = px;
        pixelY = py;
        pixelWidth = pw;
        pixelHeight = ph;
        x = static_cast<float>(px) / rasterWidth;
        y = static_cast<float>(py) / rasterHeight;
        width = static_cast<float>(pw) / rasterWidth;
        height = static_cast<float>(ph) / rasterHeight;
    }

    /**
     * Reset to full raster.
     */
    void reset() {
        x = 0.0f;
        y = 0.0f;
        width = 1.0f;
        height = 1.0f;
    }

    /**
     * Check if this is the full raster (no cropping).
     */
    bool isFullRaster() const {
        return x == 0.0f && y == 0.0f && width == 1.0f && height == 1.0f;
    }
};

/**
 * OutputDisplay component for projection mapping outputs.
 *
 * Represents a physical display (projector, LED wall, monitor) or
 * virtual output (preview, NDI) that receives mapped content.
 *
 * Each output has:
 * - Input region: Which part of the composited raster to use
 * - Output target: Physical display index or virtual output type
 * - Resolution: Native resolution of the output device
 */
struct OutputDisplay {
    // Display identification
    std::string name{"Output 1"};
    uint32_t outputIndex{0};
    OutputType type{OutputType::Physical};

    // Input source region (which part of raster feeds this output)
    InputRegion inputRegion;

    // Output resolution
    int32_t width{1920};
    int32_t height{1080};
    float refreshRate{60.0f};

    // Physical display info (for OutputType::Physical)
    int32_t physicalDisplayIndex{0};   // Index from display enumeration
    std::string deviceName;             // System device name (e.g., "\\\\.\\DISPLAY1")
    std::string displayName;            // Friendly name (e.g., "DELL U2718Q")

    // NDI output info (for OutputType::NDI)
    std::string ndiSourceName{"Entity Output"};

    // State
    bool enabled{true};
    bool fullscreen{false};

    // Window position (for windowed mode or preview)
    int32_t windowX{100};
    int32_t windowY{100};
    int32_t windowWidth{1280};
    int32_t windowHeight{720};

    // Color/gamma per output (for display calibration)
    // Pipeline ordering after Phase C.12: brightness → OCIO display+view ODT → gamma trim.
    float brightness{1.0f};
    float gamma{1.0f};

    // OCIO display + view used by mapping_surface_ps as the per-output ODT.
    // Empty = fall back to the active config's default display/view at draw time.
    std::string ocioDisplay;
    std::string ocioView;

    // Renderer slot for the physical output window/swap chain.
    // UINT32_MAX = not created yet (disabled, no display assigned, or virtual).
    // Populated by OutputManager when the output is enabled with a display assigned.
    uint32_t outputWindowSlot{UINT32_MAX};

    // Which Screen's compose target feeds this output. entt::null = use the
    // first visible Screen found in the registry (sane default when there's
    // only one). Explicit routing comes later — Phase C #1 is single-raster.
    entt::entity sourceScreen{entt::null};

    // When set, this output is driven by a Projector: the projector's target
    // screen content is rendered fullscreen to this output. Takes priority over
    // sourceScreen. entt::null = not projector-driven.
    entt::entity sourceProjector{entt::null};

    // Compose target slot for the calibration crosshair overlay. UINT32_MAX = no
    // overlay. Set by ProjectorCalibrationWindow when routing to this output;
    // OutputManager composites it on top of the projector view.
    uint32_t calibrationOverlaySlot{UINT32_MAX};

    /**
     * Get the aspect ratio of the output.
     */
    float getAspectRatio() const {
        return height > 0 ? static_cast<float>(width) / height : 1.0f;
    }

    /**
     * Check if output is a physical display.
     */
    bool isPhysical() const {
        return type == OutputType::Physical;
    }

    /**
     * Check if output is a virtual/preview output.
     */
    bool isVirtual() const {
        return type == OutputType::Preview || type == OutputType::Virtual;
    }

    /**
     * Get type as string for display.
     */
    const char* getTypeString() const {
        switch (type) {
            case OutputType::Physical: return "Physical";
            case OutputType::Preview:  return "Preview";
            case OutputType::NDI:      return "NDI";
            case OutputType::Virtual:  return "Virtual";
            default: return "Unknown";
        }
    }
};

} // namespace entity
