#pragma once

#include "../bus/IMessageTransport.hpp"
#include "../bus/Message.hpp"
#include "../core/Types.hpp"
#include "../components/OutputDisplay.hpp"
#include "../render/IRenderer.hpp"
#include <entt/entt.hpp>
#include <unordered_map>
#include <unordered_set>
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
 * - OutputSurfaces within an output define how that input is warped/mapped
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
     * Destroy every output window (show thread only). Called from the show
     * loop's launcher-idle branch: after closeProject the show thread never
     * reaches renderOutputs, so its reconciliation sweep can't fire — this
     * is the teardown path for "project closed, windows must come down".
     * No-op when no windows exist.
     */
    void destroyAllOutputWindowsOnShow();

    /**
     * Show-thread half of the SetOutputEnabled bus message (issue #76).
     * disable → destroy the output window now (prompt teardown between
     * frames, where no D3D12 work is in flight). enable → no-op: the next
     * RenderFrame's OutputSnapshot carries enabled=true (the editor wrote
     * the registry before publishing the message) and renderOutputs
     * lazily creates the window from it. Never touches the registry.
     */
    void handleSetOutputEnabledOnShow(entt::entity outputEntity, bool enabled);

    /**
     * Set output to fullscreen mode on its assigned display.
     */
    void setFullscreen(entt::entity outputEntity, bool fullscreen);

    /**
     * Render all enabled outputs.
     *
     * Called each frame AFTER the compositor has rendered to its compose
     * targets and BEFORE the ImGui overlay. Each enabled Physical output
     * gets its back buffer drawn with the composited raster cropped by
     * InputRegion.
     */
    void renderOutputs(const bus::RenderFrame& rf);

    /**
     * Get the primary preview output (if any).
     */
    entt::entity getPreviewOutput() const { return m_previewOutput; }

    /**
     * Re-sync the internal output-index counter from entities already in the
     * registry. Call after loading a project — the serializer restores each
     * output's outputIndex verbatim, so a naive auto-increment on the next
     * `+ Physical` click would collide with a loaded one.
     */
    void syncCounterFromRegistry();

    /**
     * Post-load geometry re-resolution (editor thread — legal registry
     * writes + m_availableDisplays reads). The serializer restores
     * windowX/Y/width/height verbatim from the project file, but the show
     * thread now creates windows from those baked fields (issue #76), so a
     * project saved under a different monitor layout would come up at
     * stale coordinates. For each Physical output: physicalDisplayIndex in
     * range → refresh geometry from the live display list (what the old
     * show-side ensureOutputWindow did implicitly); out of range (rig has
     * fewer displays than the save) → disable the output + warn, instead
     * of creating an off-desktop window that presents to nowhere.
     */
    void reresolveDisplayGeometry();

    /**
     * Wire in the D2R transport so assignDisplay can route swap-chain
     * teardown/rebuild through the show thread instead of calling the
     * renderer directly from the editor thread.
     */
    void setTransport(bus::IMessageTransport* transport) { m_transport = transport; }

private:

    /**
     * Render to a single output from bus snapshots.
     */
    void renderToOutput(const bus::OutputSnapshot& output,
                        uint32_t windowSlot,
                        TextureRef compositedTexture,
                        const std::vector<bus::OutputSurfaceSnapshot>& surfaces,
                        const std::vector<bus::ProjectorSnapshot>& projectors,
                        const std::vector<bus::ScreenSnapshot>& screens);

    /**
     * Resolve which compose target texture feeds a given output. Returns an
     * invalid TextureRef if no suitable Screen exists.
     */
    TextureRef resolveSourceTexture(const bus::OutputSnapshot& output,
                                    const std::vector<bus::ScreenSnapshot>& screens,
                                    const std::vector<bus::ProjectorSnapshot>& projectors) const;

    /**
     * Ensure a Physical output has an owned GLFW window + swap chain on the
     * renderer, driven entirely by the baked OutputSnapshot (registry-free
     * — issue #76; ADR-0014 forbids show-thread registry access, and window
     * geometry comes from the snapshot's windowX/Y/width/height, not the
     * editor-mutated m_availableDisplays). Returns the slot (existing or
     * newly allocated), or UINT32_MAX on failure. On creation, records the
     * slot in m_windowSlots and posts an R2D OutputWindowSlotUpdated so the
     * editor mirrors it into OutputDisplay for the UI. Show thread only.
     */
    uint32_t ensureOutputWindow(const bus::OutputSnapshot& snap);

    /**
     * Destroy the output window for an entity, if any: erase from
     * m_windowSlots, destroy the renderer window, post the R2D
     * slot-cleared mirror update. Show thread only (and shutdown, which
     * runs after the show thread has joined).
     */
    void destroyOutputWindowFor(entt::entity outputEntity);

    IRenderer* m_renderer{nullptr};
    entt::registry& m_registry;
    bus::IMessageTransport* m_transport{nullptr};

    // Available physical displays from enumeration
    std::vector<DisplayInfo> m_availableDisplays;

    // Authoritative output-window state (issue #76). SHOW-THREAD-OWNED:
    // written by renderOutputs' lazy creation / reconciliation and the
    // SetOutputEnabled disable handler; the only editor-side access is
    // shutdown(), which runs after the show thread has joined. The registry
    // field OutputDisplay::outputWindowSlot is a display-only mirror kept
    // roughly current via R2D OutputWindowSlotUpdated replies — never read
    // it to decide window lifecycle.
    //
    // Each record carries the geometry the window was created with so the
    // reconciliation sweep can destroy-and-lazily-recreate on a geometry
    // change (display reassignment). Geometry flows exclusively through the
    // snapshot — no message can race it.
    struct WindowRecord {
        uint32_t     slot{UINT32_MAX};
        std::int32_t x{0};
        std::int32_t y{0};
        std::int32_t width{0};
        std::int32_t height{0};
    };
    std::unordered_map<entt::entity, WindowRecord> m_windowSlots;

    // Disable tombstones (issue #76 review): a SetOutputEnabled(false)
    // message is consumed AFTER this tick's RenderFrame was built, so the
    // stale frame still shows the output enabled and lazy creation would
    // resurrect the just-destroyed window for a frame (or indefinitely if
    // the editor is stalled — the ESC-panic scenario). An entity in this
    // set is excluded from lazy creation until a RenderFrame that reflects
    // the disable (entity absent or enabled=false) is observed, which is
    // when the tombstone is cleared. An enable message also clears it.
    // Same ownership as m_windowSlots: show thread only.
    std::unordered_set<entt::entity> m_disableTombstones;

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
