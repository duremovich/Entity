#pragma once

#include "../core/Types.hpp"
#include "../components/OutputDisplay.hpp"
#include "../render/IRenderer.hpp"
#include <entt/entt.hpp>
#include <vector>
#include <string>
#include <memory>

namespace entity {

/**
 * Display info from system enumeration.
 */
struct DisplayInfo {
    int32_t index{0};
    std::string deviceName;     // System device name (e.g., "\\\\.\\DISPLAY1")
    std::string displayName;    // Friendly name (e.g., "DELL U2718Q")
    int32_t x{0};               // Desktop position X
    int32_t y{0};               // Desktop position Y
    int32_t width{1920};
    int32_t height{1080};
    float refreshRate{60.0f};
    bool isPrimary{false};
};

/**
 * OutputManager - Manages output displays for projection mapping.
 *
 * Handles:
 * - Enumeration of physical displays (monitors, projectors)
 * - Creation and management of output entities
 * - Rendering pipeline coordination (composite → input crop → surface mapping → output)
 * - Preview window rendering
 *
 * Architecture:
 * - Each output has an InputRegion that selects which part of the composited raster to use
 * - MappingSurfaces within an output define how that input is warped/mapped
 * - Physical outputs go to full-screen windows on specific displays
 * - Virtual outputs can be preview windows or NDI streams
 */
class OutputManager {
public:
    OutputManager(IRenderer* renderer, entt::registry& registry);
    ~OutputManager();

    /**
     * Initialize the output manager.
     * Enumerates available displays and sets up default output.
     */
    Result initialize();

    /**
     * Shutdown and cleanup resources.
     */
    void shutdown();

    /**
     * Enumerate available physical displays.
     * Call this to refresh the list of connected monitors/projectors.
     */
    void enumerateDisplays();

    /**
     * Get list of available physical displays.
     */
    const std::vector<DisplayInfo>& getAvailableDisplays() const { return m_availableDisplays; }

    /**
     * Create a new output display entity.
     *
     * @param name User-friendly name for the output
     * @param type Type of output (Physical, Preview, NDI)
     * @return Entity handle for the created output
     */
    entt::entity createOutput(const std::string& name, OutputType type = OutputType::Physical);

    /**
     * Remove an output display entity.
     */
    void removeOutput(entt::entity outputEntity);

    /**
     * Get all output entities.
     */
    std::vector<entt::entity> getOutputs() const;

    /**
     * Get number of outputs.
     */
    size_t getOutputCount() const;

    /**
     * Assign a physical display to an output.
     *
     * @param outputEntity The output entity to configure
     * @param displayIndex Index from getAvailableDisplays()
     */
    void assignDisplay(entt::entity outputEntity, int32_t displayIndex);

    /**
     * Set the input region for an output (which part of the raster it receives).
     *
     * @param outputEntity The output entity to configure
     * @param x Left edge (0-1 normalized)
     * @param y Top edge (0-1 normalized)
     * @param width Width (0-1 normalized)
     * @param height Height (0-1 normalized)
     */
    void setInputRegion(entt::entity outputEntity, float x, float y, float width, float height);

    /**
     * Set the full raster size (composition canvas size).
     * Input regions are defined as fractions of this size.
     */
    void setRasterSize(int32_t width, int32_t height);

    int32_t getRasterWidth() const { return m_rasterWidth; }
    int32_t getRasterHeight() const { return m_rasterHeight; }

    /**
     * Enable/disable an output.
     */
    void setOutputEnabled(entt::entity outputEntity, bool enabled);

    /**
     * Set output to fullscreen mode on its assigned display.
     */
    void setFullscreen(entt::entity outputEntity, bool fullscreen);

    /**
     * Render all enabled outputs.
     * Called each frame after compositing.
     *
     * @param compositedTexture Texture reference for the full composited raster
     */
    void renderOutputs(TextureRef compositedTexture);

    /**
     * Get the primary preview output (if any).
     */
    entt::entity getPreviewOutput() const { return m_previewOutput; }

private:
    /**
     * Create render resources for an output (render target, etc).
     */
    void createOutputResources(entt::entity outputEntity);

    /**
     * Release render resources for an output.
     */
    void releaseOutputResources(entt::entity outputEntity);

    /**
     * Render to a single output.
     */
    void renderToOutput(entt::entity outputEntity, TextureRef compositedTexture);

    IRenderer* m_renderer{nullptr};
    entt::registry& m_registry;

    // Available physical displays from enumeration
    std::vector<DisplayInfo> m_availableDisplays;

    // Full raster (composition canvas) size
    int32_t m_rasterWidth{1920};
    int32_t m_rasterHeight{1080};

    // Default preview output
    entt::entity m_previewOutput{entt::null};

    // Output counter for naming
    uint32_t m_outputCounter{0};

    bool m_initialized{false};
};

} // namespace entity
