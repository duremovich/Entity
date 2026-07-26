/**
 * OutputManager Implementation
 *
 * Manages output displays for projection mapping, including:
 * - Physical display enumeration (monitors, projectors)
 * - Input region selection (which part of raster feeds each output)
 * - Output rendering coordination
 */

#include "entity/render/OutputManager.hpp"
#include "entity/profile/Tracy.hpp"
#include "entity/bus/Message.hpp"
#include "entity/bus/Serialization.hpp"
#include "entity/components/Model.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <algorithm>
#include <cmath>

namespace entity {

OutputManager::OutputManager(IRenderer* renderer, entt::registry& registry)
    : m_renderer(renderer)
    , m_registry(registry)
{
}

OutputManager::~OutputManager() {
    shutdown();
}

Result OutputManager::initialize() {
    if (m_initialized) {
        return Result::Success;
    }

    std::cout << "[OutputManager] Initializing..." << std::endl;

    // Enumerate available displays
    enumerateDisplays();

    // Create default preview output
    m_previewOutput = createOutput("Preview", OutputType::Preview);
    if (m_previewOutput != entt::null) {
        auto& output = m_registry.get<OutputDisplay>(m_previewOutput);
        output.enabled = true;
        output.width = 1280;
        output.height = 720;
        std::cout << "[OutputManager] Created default preview output" << std::endl;
    }

    m_initialized = true;
    std::cout << "[OutputManager] Initialized with " << m_availableDisplays.size()
              << " physical displays" << std::endl;

    return Result::Success;
}

void OutputManager::shutdown() {
    if (!m_initialized) {
        return;
    }

    std::cout << "[OutputManager] Shutting down..." << std::endl;

    // Destroy any remaining output windows. Must happen BEFORE the renderer
    // itself shuts down (it owns the swap chains we reference by slot).
    // Driven by the show-owned slot map, not a registry scan — the registry
    // may already be cleared at this point, and shutdown runs after the
    // show thread has joined so touching m_windowSlots here is safe.
    if (m_renderer) {
        for (uint32_t slot : m_pendingWindowDestroys) {
            (void)m_renderer->destroyOutputWindow(slot);
        }
        for (const auto& [entity, rec] : m_windowSlots) {
            (void)m_renderer->destroyOutputWindow(rec.slot);
        }
    }
    m_pendingWindowDestroys.clear();
    m_windowSlots.clear();
    m_disableTombstones.clear();

    m_initialized = false;
}

void OutputManager::enumerateDisplays() {
    m_availableDisplays.clear();

#ifdef _WIN32
    // Use Windows display enumeration
    int displayIndex = 0;

    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) -> BOOL {
            auto* displays = reinterpret_cast<std::vector<DisplayInfo>*>(lParam);

            MONITORINFOEX monitorInfo{};
            monitorInfo.cbSize = sizeof(MONITORINFOEX);
            GetMonitorInfo(hMonitor, &monitorInfo);

            DisplayInfo info;
            info.index = static_cast<int32_t>(displays->size());
            info.deviceName = monitorInfo.szDevice;
            info.x = monitorInfo.rcMonitor.left;
            info.y = monitorInfo.rcMonitor.top;
            info.width = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
            info.height = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;
            info.isPrimary = (monitorInfo.dwFlags & MONITORINFOF_PRIMARY) != 0;

            // Try to get friendly display name
            DISPLAY_DEVICE displayDevice{};
            displayDevice.cb = sizeof(DISPLAY_DEVICE);
            if (EnumDisplayDevices(monitorInfo.szDevice, 0, &displayDevice, 0)) {
                info.displayName = displayDevice.DeviceString;
            } else {
                info.displayName = "Display " + std::to_string(info.index + 1);
            }

            // Get refresh rate from current display settings
            DEVMODE devMode{};
            devMode.dmSize = sizeof(DEVMODE);
            if (EnumDisplaySettings(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode)) {
                info.refreshRate = static_cast<float>(devMode.dmDisplayFrequency);
            }

            displays->push_back(info);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&m_availableDisplays)
    );

    // Log enumerated displays
    for (const auto& display : m_availableDisplays) {
        std::cout << "[OutputManager] Display " << display.index << ": "
                  << display.displayName << " (" << display.deviceName << ") "
                  << display.width << "x" << display.height << " @ "
                  << display.refreshRate << "Hz"
                  << (display.isPrimary ? " [PRIMARY]" : "") << std::endl;
    }
#else
    // Fallback: create a single default display entry
    DisplayInfo defaultDisplay;
    defaultDisplay.index = 0;
    defaultDisplay.deviceName = "DISPLAY0";
    defaultDisplay.displayName = "Default Display";
    defaultDisplay.width = 1920;
    defaultDisplay.height = 1080;
    defaultDisplay.isPrimary = true;
    m_availableDisplays.push_back(defaultDisplay);
#endif
}

entt::entity OutputManager::createOutput(const std::string& name, OutputType type) {
    entt::entity entity = m_registry.create();

    auto& output = m_registry.emplace<OutputDisplay>(entity);
    output.name = name;
    output.outputIndex = m_outputCounter++;
    output.type = type;

    // Set default resolution based on type
    if (type == OutputType::Preview) {
        output.width = 1280;
        output.height = 720;
    } else if (type == OutputType::Physical && !m_availableDisplays.empty()) {
        // Use primary display resolution as default
        for (const auto& display : m_availableDisplays) {
            if (display.isPrimary) {
                output.width = display.width;
                output.height = display.height;
                output.refreshRate = display.refreshRate;
                break;
            }
        }
    }

    // Initialize input region with pixel values
    output.inputRegion.updatePixelCoords(m_rasterWidth, m_rasterHeight);

    std::cout << "[OutputManager] Created output '" << name << "' (type: "
              << output.getTypeString() << ", index: " << output.outputIndex << ")" << std::endl;

    return entity;
}

void OutputManager::removeOutput(entt::entity outputEntity) {
    if (!m_registry.valid(outputEntity)) {
        return;
    }

    if (outputEntity == m_previewOutput) {
        m_previewOutput = entt::null;
    }

    // Window teardown is NOT done here (editor thread): the entity vanishes
    // from the next RenderFrame's outputs, and renderOutputs' reconciliation
    // sweep destroys the orphaned window on the show thread a frame later
    // (issue #76 — window lifecycle is show-thread-owned).

    if (m_registry.all_of<OutputDisplay>(outputEntity)) {
        auto& output = m_registry.get<OutputDisplay>(outputEntity);
        std::cout << "[OutputManager] Removing output '" << output.name << "'" << std::endl;
    }

    m_registry.destroy(outputEntity);
}

std::vector<entt::entity> OutputManager::getOutputs() const {
    std::vector<entt::entity> outputs;
    auto view = m_registry.view<OutputDisplay>();
    for (auto entity : view) {
        outputs.push_back(entity);
    }
    return outputs;
}

size_t OutputManager::getOutputCount() const {
    auto view = m_registry.view<OutputDisplay>();
    size_t count = 0;
    for (auto entity : view) {
        (void)entity;
        count++;
    }
    return count;
}

void OutputManager::assignDisplay(entt::entity outputEntity, int32_t displayIndex) {
    if (!m_registry.valid(outputEntity) || !m_registry.all_of<OutputDisplay>(outputEntity)) {
        return;
    }

    if (displayIndex < 0 || displayIndex >= static_cast<int32_t>(m_availableDisplays.size())) {
        std::cerr << "[OutputManager] Invalid display index: " << displayIndex << std::endl;
        return;
    }

    auto& output = m_registry.get<OutputDisplay>(outputEntity);
    const auto& display = m_availableDisplays[displayIndex];

    std::cout << "[OutputManager] Assigned display '" << display.displayName
              << "' to output '" << output.name << "'" << std::endl;

    // Update registry fields on the editor thread (sole registry writer).
    // That is ALL assignDisplay does since issue #76: the next SceneSnapshot
    // bakes the new geometry into the OutputSnapshot, and the show thread's
    // reconciliation sweep destroys-and-recreates the window when it sees
    // the geometry change. No bus message — an explicit disable here would
    // race the snapshot (the message always arrives a frame earlier, and a
    // lazy recreate off the stale frame would pin the window at the OLD
    // geometry forever). Geometry flows through exactly one ordered
    // channel: the snapshot.
    output.physicalDisplayIndex = displayIndex;
    output.deviceName  = display.deviceName;
    output.displayName = display.displayName;
    output.width       = display.width;
    output.height      = display.height;
    output.refreshRate = display.refreshRate;
    output.windowX     = display.x;
    output.windowY     = display.y;
}

void OutputManager::reresolveDisplayGeometry() {
    // Editor thread, post-load. The serializer restores windowX/Y/width/
    // height verbatim from the project file, but windows are created from
    // those baked fields since issue #76 — re-anchor them to the CURRENT
    // machine's display layout (the old show-side ensureOutputWindow did
    // this implicitly by reading m_availableDisplays at create time).
    auto view = m_registry.view<OutputDisplay>();
    for (auto [entity, out] : view.each()) {
        if (!out.isPhysical()) continue;
        if (out.physicalDisplayIndex < 0) continue;
        if (out.physicalDisplayIndex <
                static_cast<int32_t>(m_availableDisplays.size())) {
            const DisplayInfo& d = m_availableDisplays[out.physicalDisplayIndex];
            out.deviceName  = d.deviceName;
            out.displayName = d.displayName;
            out.width       = d.width;
            out.height      = d.height;
            out.refreshRate = d.refreshRate;
            out.windowX     = d.x;
            out.windowY     = d.y;
        } else if (out.enabled) {
            // Saved on a rig with more displays than this machine has.
            // Pre-#76 the window silently never came up (index bounds check
            // at create time); disabling is the same net effect but visible
            // in the UI instead of an enabled output that displays nowhere.
            std::cerr << "[OutputManager] Output '" << out.name
                      << "' assigned to display " << out.physicalDisplayIndex
                      << " but only " << m_availableDisplays.size()
                      << " display(s) present — disabling" << std::endl;
            out.enabled = false;
        }
    }
}

void OutputManager::setInputRegion(entt::entity outputEntity, float x, float y, float width, float height) {
    if (!m_registry.valid(outputEntity) || !m_registry.all_of<OutputDisplay>(outputEntity)) {
        return;
    }

    auto& output = m_registry.get<OutputDisplay>(outputEntity);
    output.inputRegion.x = std::clamp(x, 0.0f, 1.0f);
    output.inputRegion.y = std::clamp(y, 0.0f, 1.0f);
    output.inputRegion.width = std::clamp(width, 0.0f, 1.0f - output.inputRegion.x);
    output.inputRegion.height = std::clamp(height, 0.0f, 1.0f - output.inputRegion.y);

    // Update pixel coordinates
    output.inputRegion.updatePixelCoords(m_rasterWidth, m_rasterHeight);

    std::cout << "[OutputManager] Set input region for '" << output.name << "': "
              << output.inputRegion.pixelX << "," << output.inputRegion.pixelY << " "
              << output.inputRegion.pixelWidth << "x" << output.inputRegion.pixelHeight << std::endl;
}

void OutputManager::setRasterSize(int32_t width, int32_t height) {
    m_rasterWidth = width;
    m_rasterHeight = height;

    // Update all output input regions with new pixel coords
    auto view = m_registry.view<OutputDisplay>();
    for (auto [entity, output] : view.each()) {
        output.inputRegion.updatePixelCoords(m_rasterWidth, m_rasterHeight);
    }

    std::cout << "[OutputManager] Raster size set to " << width << "x" << height << std::endl;
}

void OutputManager::destroyAllOutputWindowsOnShow() {
    if (m_windowSlots.empty()) return;
    std::vector<entt::entity> all;
    all.reserve(m_windowSlots.size());
    for (const auto& [entity, slot] : m_windowSlots) {
        all.push_back(entity);
    }
    for (entt::entity entity : all) {
        destroyOutputWindowFor(entity);
    }
}

void OutputManager::handleSetOutputEnabledOnShow(entt::entity outputEntity,
                                                 bool enabled) {
    // Show-thread half of the SetOutputEnabled message (issue #76). The
    // editor already wrote OutputDisplay::enabled into the registry before
    // publishing (publishSetOutputEnabled chokepoint), so this handler owns
    // window lifecycle only — zero registry access per ADR-0014.
    if (!enabled) {
        destroyOutputWindowFor(outputEntity);
        // Tombstone: this tick's RenderFrame was built before the message
        // was drained, so it still shows the output enabled — without the
        // tombstone, renderOutputs would resurrect the window this same
        // tick (one-frame flash on routine disables; indefinitely if the
        // editor is stalled, defeating the ESC panic stop). Cleared by the
        // reconciliation sweep once a frame reflects the disable.
        m_disableTombstones.insert(outputEntity);
    } else {
        // enable → lift any tombstone; window creation itself is lazy (the
        // next RenderFrame that shows the output enabled brings it up).
        m_disableTombstones.erase(outputEntity);
    }
    std::cout << "[OutputManager] Output "
              << static_cast<uint32_t>(outputEntity) << " "
              << (enabled ? "enable (lazy window creation)" : "disabled")
              << std::endl;
}

void OutputManager::setFullscreen(entt::entity outputEntity, bool fullscreen) {
    if (!m_registry.valid(outputEntity) || !m_registry.all_of<OutputDisplay>(outputEntity)) {
        return;
    }

    auto& output = m_registry.get<OutputDisplay>(outputEntity);
    output.fullscreen = fullscreen;

    // TODO: Actually toggle fullscreen window if we have a window handle

    std::cout << "[OutputManager] Output '" << output.name << "' fullscreen: "
              << (fullscreen ? "on" : "off") << std::endl;
}

// Marker-test overlay (TROUBLESHOOTING.md "Output Content Appears Frozen
// During Editor Drag / Resize / Modal"). Set ENTITY_MARKER_TEST=1 to draw a
// 20%-size quad in the bottom-right corner of every physical output. Color
// cycles across 8 hues on rf.frameNumber (Timeline current frame). Regular
// content keeps rendering underneath — the corner strobe and the user's own
// content frame counter are independent signals:
//   strobe ticks normally + content advances → all clear
//   strobe ticks normally + content frozen   → freeze is downstream of Timeline
//                                              (decode workers / FrameCache /
//                                              PlaybackPresenter skip)
//   strobe slows or freezes + content frozen → Timeline itself isn't
//                                              advancing on the show thread
static std::uint64_t s_markerTimelineFrame = 0;
static bool markerTestEnabled() {
    static const bool s_enabled = []() {
        const char* v = std::getenv("ENTITY_MARKER_TEST");
        return v && v[0] != '0' && v[0] != '\0';
    }();
    return s_enabled;
}
static void drawMarkerOverlay(IRenderer* renderer) {
    if (!markerTestEnabled() || !renderer) return;
    const std::uint64_t phase = s_markerTimelineFrame & 0x7ULL;
    glm::vec4 color(0.0f, 0.0f, 0.0f, 1.0f);
    switch (phase) {
        case 0: color.r = 1.0f; break;                              // red
        case 1: color.r = 1.0f; color.g = 0.5f; break;              // orange
        case 2: color.r = color.g = 1.0f; break;                    // yellow
        case 3: color.g = 1.0f; break;                              // green
        case 4: color.g = color.b = 1.0f; break;                    // cyan
        case 5: color.b = 1.0f; break;                              // blue
        case 6: color.r = color.b = 1.0f; break;                    // magenta
        case 7: color.r = color.g = color.b = 1.0f; break;          // white
    }
    // 20% size, bottom-right corner. NDC: x=[0.6, 1.0], y=[-1.0, -0.6].
    glm::mat4 T(1.0f);
    T = glm::translate(T, glm::vec3(0.8f, -0.8f, 0.0f));
    T = glm::scale(T, glm::vec3(0.2f, 0.2f, 1.0f));
    renderer->drawColoredQuad(T, color, 1.0f);
}

void OutputManager::renderOutputs(const bus::RenderFrame& rf) {
    ZoneScopedN("OutputManager::renderOutputs");
    if (!m_initialized || !m_renderer) {
        return;
    }

    // Marker-test mode (TROUBLESHOOTING.md "Output Content Appears Frozen
    // During Editor Drag / Resize / Modal"). Set ENTITY_MARKER_TEST=1 to
    // overlay a 20% corner quad on each physical output, color-cycling on
    // rf.frameNumber. Regular content rendering still runs underneath, so
    // the user can compare the strobe (Timeline) against their own content's
    // frame counter (decode pipeline). See drawMarkerOverlay() above.
    s_markerTimelineFrame = rf.frameNumber;
    if (markerTestEnabled()) {
        static uint64_t s_showCounter = 0;
        ++s_showCounter;
        // Log every 10 show frames for finer-grained pace evidence.
        if ((s_showCounter % 10) == 0) {
            std::cerr << "[MARKER] showFrame=" << s_showCounter
                      << " rf.frameNumber=" << rf.frameNumber
                      << " playState=" << static_cast<int>(rf.playState)
                      << std::endl;
        }
    }

    // Reconciliation sweep (issue #76): destroy windows whose entity no
    // longer appears in this frame's outputs as an enabled Physical output
    // — covers output deletion, project load/close (old entities vanish
    // from the post-load snapshot), and disables whose bus message was
    // superseded — or whose snapshot geometry no longer matches what the
    // window was created with (display reassignment: destroy here, the
    // lazy-create loop below recreates with THIS frame's new geometry in
    // the same tick). This is what lets the editor thread never touch
    // window lifecycle: the snapshot IS the lifecycle signal.
    //
    // The sweep also clears disable tombstones once the frame reflects the
    // disable — until then, lazy creation must not resurrect a window whose
    // disable message out-ran the snapshot (the stale frame still says
    // enabled; with a stalled editor that staleness can last seconds — the
    // ESC-panic scenario).
    // #91: retry destroys deferred by an abandoned GPU drain. Each failed
    // attempt blocks the show thread up to the fence timeout, so cap the
    // per-tick cost at one attempt: retry only the FRONT pending slot, and on
    // failure leave it (and don't try the others this tick). The non-blocking
    // fence-poll redesign is tracked in the #91 follow-up.
    if (!m_pendingWindowDestroys.empty() && m_renderer) {
        if (m_renderer->destroyOutputWindow(m_pendingWindowDestroys.front())) {
            m_pendingWindowDestroys.erase(m_pendingWindowDestroys.begin());
        }
    }

    if (!m_windowSlots.empty() || !m_disableTombstones.empty()) {
        std::vector<entt::entity> stale;
        for (const auto& [ent, rec] : m_windowSlots) {
            const bus::OutputSnapshot* live = nullptr;
            for (const auto& snap : rf.outputs) {
                if (static_cast<entt::entity>(snap.entity) == ent) {
                    if (snap.enabled && snap.isPhysical) live = &snap;
                    break;
                }
            }
            const bool geometryChanged = live &&
                (live->windowX != rec.x || live->windowY != rec.y ||
                 live->width   != rec.width || live->height != rec.height);
            if (!live || geometryChanged) stale.push_back(ent);
        }
        for (entt::entity ent : stale) {
            destroyOutputWindowFor(ent);
        }

        std::vector<entt::entity> served;
        for (entt::entity ent : m_disableTombstones) {
            bool stillEnabledInFrame = false;
            for (const auto& snap : rf.outputs) {
                if (static_cast<entt::entity>(snap.entity) == ent) {
                    stillEnabledInFrame = snap.enabled && snap.isPhysical;
                    break;
                }
            }
            if (!stillEnabledInFrame) served.push_back(ent);
        }
        for (entt::entity ent : served) {
            m_disableTombstones.erase(ent);
        }
    }

    for (const auto& snap : rf.outputs) {
        if (!snap.enabled || !snap.isPhysical) {
            continue;
        }

        const entt::entity entity = static_cast<entt::entity>(snap.entity);

        // A tombstoned entity was disabled by a message this frame hasn't
        // caught up with — do not resurrect its window off the stale frame.
        if (m_disableTombstones.count(entity)) continue;

        // Window lifecycle is keyed off the show-owned slot map, NOT
        // snap.outputWindowSlot — the registry field is a display mirror
        // that lags by the R2D reply latency, and trusting it here would
        // double-create during that window (the original #76 failure).
        // ensureOutputWindow returns the existing slot or lazily creates —
        // covering enables, deserialized projects, and the same-tick
        // recreate after a geometry-change destroy above.
        const uint32_t windowSlot = ensureOutputWindow(snap);
        if (windowSlot == UINT32_MAX) continue;

        TextureRef source = resolveSourceTexture(snap, rf.screens, rf.projectors);
        if (!source.valid()) {
            m_renderer->beginOutputFrame(windowSlot);
            m_renderer->clearOutputFrame(windowSlot, 0.0f, 0.0f, 0.0f, 1.0f);
            drawMarkerOverlay(m_renderer);
            m_renderer->endOutputFrame(windowSlot);
            continue;
        }

        renderToOutput(snap, windowSlot, source, rf.surfaces, rf.projectors, rf.screens);
    }
}

void OutputManager::destroyOutputWindowFor(entt::entity outputEntity) {
    auto it = m_windowSlots.find(outputEntity);
    if (it == m_windowSlots.end()) return;

    const uint32_t slot = it->second.slot;
    m_windowSlots.erase(it);
    if (m_renderer && !m_renderer->destroyOutputWindow(slot)) {
        // #91: deferred — the renderer kept the slot active. Retry each tick;
        // the map entry is already erased so the window is logically gone.
        m_pendingWindowDestroys.push_back(slot);
    }
    // Clear the editor-side registry mirror via R2D (the entity may already
    // be destroyed — the editor drain validates before writing).
    if (m_transport) {
        bus::OutputWindowSlotUpdated reply{
            static_cast<std::uint64_t>(outputEntity), UINT32_MAX};
        m_transport->send(bus::Direction::R2D,
                          bus::serialize(bus::Message{reply}));
    }
    std::cout << "[OutputManager] Destroyed output window slot " << slot
              << " for entity " << static_cast<uint32_t>(outputEntity)
              << std::endl;
}

void OutputManager::syncCounterFromRegistry() {
    uint32_t maxIdx = 0;
    bool any = false;
    auto view = m_registry.view<OutputDisplay>();
    for (auto [entity, out] : view.each()) {
        any = true;
        if (out.outputIndex >= maxIdx) maxIdx = out.outputIndex;
    }
    m_outputCounter = any ? (maxIdx + 1) : 0;
    std::cout << "[OutputManager] Counter synced to " << m_outputCounter
              << " (from " << (any ? "registry state" : "empty registry") << ")" << std::endl;
}

uint32_t OutputManager::ensureOutputWindow(const bus::OutputSnapshot& snap) {
    if (!m_renderer) return UINT32_MAX;

    const entt::entity entity = static_cast<entt::entity>(snap.entity);
    if (auto it = m_windowSlots.find(entity); it != m_windowSlots.end()) {
        return it->second.slot;
    }
    if (!snap.isPhysical || !snap.enabled) return UINT32_MAX;
    if (snap.physicalDisplayIndex < 0) return UINT32_MAX;
    if (snap.width <= 0 || snap.height <= 0) return UINT32_MAX;

    // Geometry comes from the baked snapshot (windowX/Y/width/height were
    // written by assignDisplay on the editor thread), NOT from
    // m_availableDisplays — the display list is editor-mutated
    // (enumerateDisplays) and reading it here would be a cross-thread race.
    std::string title = "Entity Output — " + snap.name;
    uint32_t slot = m_renderer->createOutputWindow(
        title.c_str(),
        snap.windowX, snap.windowY,
        static_cast<uint32_t>(snap.width),
        static_cast<uint32_t>(snap.height));

    if (slot == UINT32_MAX) {
        std::cerr << "[OutputManager] Failed to create output window for '"
                  << snap.name << "'" << std::endl;
        return UINT32_MAX;
    }

    m_windowSlots[entity] =
        WindowRecord{slot, snap.windowX, snap.windowY, snap.width, snap.height};

    // Mirror the slot into the registry via R2D so the OutputsWindow UI and
    // editor-side reads see it (display-only; this map stays authoritative).
    if (m_transport) {
        bus::OutputWindowSlotUpdated reply{snap.entity, slot};
        m_transport->send(bus::Direction::R2D,
                          bus::serialize(bus::Message{reply}));
    }

    std::cout << "[OutputManager] Output '" << snap.name
              << "' window up at " << snap.windowX << "," << snap.windowY
              << " " << snap.width << "x" << snap.height
              << " (slot " << slot << ")" << std::endl;
    return slot;
}

// ---------------------------------------------------------------------------
// Projector view helpers
// ---------------------------------------------------------------------------

// Build a projector VP matrix from Projector params and output aspect ratio.
// Convention: rotation[0]=pitch(X), rotation[1]=yaw(Y), rotation[2]=roll(Z).
// Forward vector matches Stage3DRenderer::buildProjectorCamera(); roll is
// applied by rotating the up vector around the forward axis.
static glm::mat4 buildProjectorVP(const bus::ProjectorSnapshot& proj, float aspect) {
    float pitch = glm::radians(proj.rotation[0]);
    float yaw   = glm::radians(proj.rotation[1]);
    float roll  = glm::radians(proj.rotation[2]);
    glm::vec3 pos(proj.position[0], proj.position[1], proj.position[2]);
    glm::vec3 fwd(
        -std::sin(yaw) * std::cos(pitch),
         std::sin(pitch),
        -std::cos(yaw) * std::cos(pitch)
    );
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(roll) > 1e-5f) {
        glm::vec3 right = glm::normalize(glm::cross(fwd, up));
        up = glm::normalize(std::cos(roll) * up + std::sin(roll) * right);
    }
    glm::mat4 view = glm::lookAt(pos, pos + fwd, up);
    glm::mat4 projM = glm::perspective(glm::radians(proj.fovDegrees), aspect,
                                       proj.nearClip, proj.farClip);
    return projM * view;
}

// Compute the 4 world-space corners of a Screen's projection surface.
// When the screen has a mesh, uses the mesh's local-space XY bounds; otherwise
// falls back to Stage3DRenderer's default 16:9 × 1 quad. This matches what the
// user actually sees in the calibration window's right pane.
// Order: [0]=TL, [1]=TR, [2]=BR, [3]=BL (drawOutputSurface convention).
static void computeScreenWorldCorners(const bus::ScreenSnapshot& screen,
                                      const MeshData* mesh,
                                      glm::vec3 corners[4]) {
    static constexpr float kScreenElevation = 0.5f; // matches Stage3DRenderer

    // Local-space rectangle to project. Default = 16:9 × 1 (Stage3DRenderer flat quad).
    float minX, maxX, minY, maxY, midZ;
    if (mesh && mesh->isValid()) {
        minX = mesh->minBounds[0]; maxX = mesh->maxBounds[0];
        minY = mesh->minBounds[1]; maxY = mesh->maxBounds[1];
        midZ = 0.5f * (mesh->minBounds[2] + mesh->maxBounds[2]);
    } else {
        const float kHW = (16.0f / 9.0f) * 0.5f;
        const float kHH = 0.5f;
        minX = -kHW; maxX = kHW;
        minY = -kHH; maxY = kHH;
        midZ = 0.0f;
    }

    glm::mat4 T = glm::mat4(1.0f);
    T = glm::translate(T, glm::vec3(screen.position[0],
                                    screen.position[1] + kScreenElevation,
                                    screen.position[2]));
    T = glm::rotate(T, glm::radians(screen.rotation[1]), glm::vec3(0.0f, 1.0f, 0.0f)); // Yaw
    T = glm::rotate(T, glm::radians(screen.rotation[0]), glm::vec3(1.0f, 0.0f, 0.0f)); // Pitch
    T = glm::rotate(T, glm::radians(screen.rotation[2]), glm::vec3(0.0f, 0.0f, 1.0f)); // Roll
    T = glm::scale(T, glm::vec3(screen.scale[0], screen.scale[1], screen.scale[2]));

    auto tp = [&T, midZ](float x, float y) {
        return glm::vec3(T * glm::vec4(x, y, midZ, 1.0f));
    };
    corners[0] = tp(minX, maxY);  // TL
    corners[1] = tp(maxX, maxY);  // TR
    corners[2] = tp(maxX, minY);  // BR
    corners[3] = tp(minX, minY);  // BL
}

// Draw a thin colored line between two NDC points using drawColoredQuad.
// Currently unused (the per-triangle mesh render replaced the wireframe diagnostic),
// kept here in case future code wants a colored-line primitive in the output frame.
[[maybe_unused]] static void drawNdcLine(IRenderer* renderer,
                                          const glm::vec2& a, const glm::vec2& b,
                                          const glm::vec4& color, float thicknessNdc = 0.005f) {
    glm::vec2 mid  = (a + b) * 0.5f;
    glm::vec2 diff = b - a;
    float len = glm::length(diff);
    if (len < 1e-5f) return;
    float angle = std::atan2(diff.y, diff.x);

    glm::mat4 T(1.0f);
    T = glm::translate(T, glm::vec3(mid, 0.0f));
    T = glm::rotate(T, angle, glm::vec3(0.0f, 0.0f, 1.0f));
    T = glm::scale(T, glm::vec3(len * 0.5f, thicknessNdc * 0.5f, 1.0f));
    renderer->drawColoredQuad(T, color, 1.0f);
}

TextureRef OutputManager::resolveSourceTexture(
        const bus::OutputSnapshot& output,
        const std::vector<bus::ScreenSnapshot>& screens,
        const std::vector<bus::ProjectorSnapshot>& projectors) const {
    auto findScreenByEntity = [&](std::uint64_t entityId) -> const bus::ScreenSnapshot* {
        for (const auto& s : screens)
            if (s.entity == entityId) return &s;
        return nullptr;
    };
    auto textureForScreen = [&](const bus::ScreenSnapshot& s) -> TextureRef {
        if (s.renderTargetValid && s.renderTargetSlot != UINT32_MAX)
            return m_renderer->getComposeTargetTexture(s.renderTargetSlot);
        return TextureRef::invalid();
    };

    // Projector source: find the projector's target screen(s). Used only as the
    // "anything renderable?" gate — the projector branch of renderToOutput
    // resolves a texture per target screen itself. Scan all explicit targets so
    // a project where targetSurfaces[0]'s compose target hasn't been allocated
    // yet but [1] has still gates green.
    if (output.sourceProjector != 0) {
        for (const auto& proj : projectors) {
            if (proj.entity != output.sourceProjector) continue;
            if (proj.targetSurfaceCount > 0) {
                for (int i = 0; i < proj.targetSurfaceCount; ++i) {
                    const auto eid = proj.targetSurfaces[i];
                    if (eid == 0) continue;
                    if (const auto* s = findScreenByEntity(eid)) {
                        auto ref = textureForScreen(*s);
                        if (ref.valid()) return ref;
                    }
                }
                return TextureRef::invalid();
            }
            // No explicit targets: fall back to first visible screen with a
            // live compose target. (Doc-comment intent on Projector::targetSurfaces
            // is "all visible ProjectionSurfaces"; the per-screen draw loop in
            // renderToOutput preserves today's first-visible behavior for the
            // count == 0 path — separate issue.)
            for (const auto& s : screens)
                if (s.visible && s.renderTargetValid && s.renderTargetSlot != UINT32_MAX)
                    return textureForScreen(s);
            return TextureRef::invalid();
        }
    }

    // Explicit route: output.sourceScreen.
    if (output.sourceScreen != 0) {
        if (const auto* s = findScreenByEntity(output.sourceScreen))
            return textureForScreen(*s);
    }

    // Fallback: first visible Screen with a live compose target.
    for (const auto& s : screens) {
        if (!s.visible) continue;
        auto ref = textureForScreen(s);
        if (ref.valid()) return ref;
    }

    return TextureRef::invalid();
}

void OutputManager::renderToOutput(
        const bus::OutputSnapshot& output,
        uint32_t windowSlot,
        TextureRef compositedTexture,
        const std::vector<bus::OutputSurfaceSnapshot>& surfaces,
        const std::vector<bus::ProjectorSnapshot>& projectors,
        const std::vector<bus::ScreenSnapshot>& screens) {
    if (!m_renderer || !compositedTexture.valid()) return;

    m_renderer->beginOutputFrame(windowSlot);
    m_renderer->clearOutputFrame(windowSlot, 0.0f, 0.0f, 0.0f, 1.0f);

    // Projector-linked output: render the screen content as seen from the projector's
    // camera. Project the screen's 3D corners through the projector VP to get NDC
    // positions, then use drawOutputSurface for perspective-correct warping.
    if (output.sourceProjector != 0) {
        // Find projector snapshot
        const bus::ProjectorSnapshot* projPtr = nullptr;
        for (const auto& p : projectors)
            if (p.entity == output.sourceProjector) { projPtr = &p; break; }

        if (projPtr) {
        const auto& proj = *projPtr;

        // Build the list of target screens. Explicit targets in
        // Projector::targetSurfaces win; if none are set, fall back to the
        // first visible screen (preserving today's count==0 behavior — the
        // doc-comment intent of "illuminate all visible ProjectionSurfaces"
        // is a separate latent fix).
        std::vector<const bus::ScreenSnapshot*> targets;
        if (proj.targetSurfaceCount > 0) {
            targets.reserve(proj.targetSurfaceCount);
            for (int i = 0; i < proj.targetSurfaceCount; ++i) {
                const auto eid = proj.targetSurfaces[i];
                if (eid == 0) continue;
                for (const auto& s : screens)
                    if (s.entity == eid) { targets.push_back(&s); break; }
            }
        } else {
            for (const auto& s : screens)
                if (s.visible) { targets.push_back(&s); break; }
        }

        if (!targets.empty()) {
            // ========================================================
            // Projector mesh-render pipeline: pose → per-vertex k1/k2
            // lens distortion → optional IDW residual warp → triangle.
            //
            // The distortion step is load-bearing — see ADR-0011 for
            // the framebuffer↔world chain. Skipping it here makes every
            // vertex land off by the lens distortion amount even before
            // the warp runs, which manifests as "warp doesn't quite hit
            // the cal points".
            //
            // Projector-global state (VP, distortion params, residuals,
            // warp) is hoisted ABOVE the per-target loop — one physical
            // projector has one lens, so the same math drives every
            // target screen. Per-target work below: resolve THIS screen's
            // compose-target texture + mesh, draw it.
            // ========================================================
            float aspect = output.height > 0 ? static_cast<float>(output.width) / output.height
                                              : 16.0f / 9.0f;
            glm::mat4 vp = buildProjectorVP(proj, aspect);

            // Lens distortion is applied per-vertex below (matches the
            // solver's pinhole+k1+k2 model). Pre-compute the projection
            // scale factors so both the residual-computation pass and
            // the per-triangle pass use identical math.
            const float tanHalfFov = std::tan(glm::radians(proj.fovDegrees) * 0.5f);
            const float sx = aspect * tanHalfFov;
            const float sy = tanHalfFov;
            const bool hasDistortion = (proj.distortionK1 != 0.0f ||
                                        proj.distortionK2 != 0.0f);

            auto applyDistortionNDC = [&](glm::vec2 ndc) -> glm::vec2 {
                if (!hasDistortion) return ndc;
                float nx = ndc.x * sx;
                float ny = ndc.y * sy;
                float r2 = nx * nx + ny * ny;
                float s  = 1.0f + proj.distortionK1 * r2
                                + proj.distortionK2 * r2 * r2;
                nx *= s; ny *= s;
                return glm::vec2(nx / sx, ny / sy);
            };

            // Optional: post-fit residual warp via inverse-distance
            // weighting of calibration-point residuals. For each cal
            // point, compute (predicted UV via the calibrated
            // pose+distortion, residual to user's measured UV). For
            // every mesh-triangle vertex's projected UV, blend the
            // residuals weighted by 1/distance². The result is a smooth
            // 2D warp that lands every calibration point EXACTLY on
            // its measured UV. Cal points are projector-global (live
            // on the Projector component, not per-Screen), so this
            // pass runs once regardless of target count.
            struct ResidualSample {
                glm::vec2 predUV;
                glm::vec2 residual;
            };
            std::vector<ResidualSample> residuals;
            const bool warpEnabled = proj.useResidualWarp &&
                                     proj.isCalibrated &&
                                     !proj.calibrationPoints.empty();
            if (warpEnabled) {
                residuals.reserve(proj.calibrationPoints.size());
                for (const auto& cp : proj.calibrationPoints) {
                    glm::vec3 wp(cp.worldPos[0], cp.worldPos[1], cp.worldPos[2]);
                    glm::vec4 clip = vp * glm::vec4(wp, 1.0f);
                    if (clip.w < 1e-5f) continue;

                    // predUV must be in the SAME framebuffer space the
                    // mesh vertices land in after rendering — i.e. with
                    // distortion applied (matches the per-vertex distort
                    // step below). Otherwise the warp leaves a residual
                    // equal to the lens distortion at every cal point.
                    glm::vec2 ndc = applyDistortionNDC(
                        glm::vec2(clip.x / clip.w, clip.y / clip.w));

                    glm::vec2 predUV((ndc.x + 1.0f) * 0.5f,
                                     (1.0f - ndc.y) * 0.5f);
                    glm::vec2 measUV(cp.projectorUV[0], cp.projectorUV[1]);
                    residuals.push_back({predUV, measUV - predUV});
                }
            }

            auto applyWarp = [&](glm::vec2 uv) -> glm::vec2 {
                if (residuals.empty()) return uv;
                glm::vec2 weighted(0.0f);
                float wSum = 0.0f;
                for (const auto& r : residuals) {
                    glm::vec2 d = uv - r.predUV;
                    float dist2 = d.x * d.x + d.y * d.y;
                    if (dist2 < 1e-8f) return uv + r.residual;
                    // 1/dist² gives strong locality near control points,
                    // smooth blend at distance. +epsilon prevents division
                    // blowup right at a control point.
                    float w = 1.0f / (dist2 + 1e-6f);
                    weighted += r.residual * w;
                    wSum += w;
                }
                if (wSum < 1e-6f) return uv;
                return uv + weighted / wSum;
            };

            // Per-target draw loop. Each screen contributes its own
            // compose-target texture and mesh. If a target has no live
            // compose target, skip it (the other targets still draw).
            for (const auto* screenSnap : targets) {
                const auto& screen = *screenSnap;

                TextureRef screenTexture = TextureRef::invalid();
                if (screen.renderTargetValid && screen.renderTargetSlot != UINT32_MAX)
                    screenTexture = m_renderer->getComposeTargetTexture(screen.renderTargetSlot);
                if (!screenTexture.valid()) continue;

                // Resolve mesh — when present, render per-triangle so the
                // projector output matches what the calibration window's
                // right pane shows. Mesh data stays in the registry
                // (read-only, read-mostly). Stage 4 will move this to a
                // renderer-side mesh store via bus::ProvisionMesh.
                const MeshData* mesh = nullptr;
                {
                    const entt::entity modelEnt = static_cast<entt::entity>(screen.modelEntity);
                    if (screen.modelEntity != 0 &&
                        m_registry.valid(modelEnt) &&
                        m_registry.all_of<Model>(modelEnt)) {
                        const auto& model = m_registry.get<Model>(modelEnt);
                        if (model.mesh.isValid()) mesh = &model.mesh;
                    }
                }

                if (mesh) {
                    glm::mat4 model_mat(1.0f);
                    static constexpr float kScreenElevation = 0.5f;
                    model_mat = glm::translate(model_mat, glm::vec3(
                        screen.position[0],
                        screen.position[1] + kScreenElevation,
                        screen.position[2]));
                    model_mat = glm::rotate(model_mat, glm::radians(screen.rotation[1]), glm::vec3(0,1,0));
                    model_mat = glm::rotate(model_mat, glm::radians(screen.rotation[0]), glm::vec3(1,0,0));
                    model_mat = glm::rotate(model_mat, glm::radians(screen.rotation[2]), glm::vec3(0,0,1));
                    model_mat = glm::scale(model_mat, glm::vec3(screen.scale[0], screen.scale[1], screen.scale[2]));

                    glm::mat4 mvp = vp * model_mat;

                    const auto& verts   = mesh->vertices;
                    const auto& indices = mesh->indices;

                    for (size_t t = 0; t + 2 < indices.size(); t += 3) {
                        const auto& v0 = verts[indices[t]];
                        const auto& v1 = verts[indices[t + 1]];
                        const auto& v2 = verts[indices[t + 2]];

                        glm::vec4 c0 = mvp * glm::vec4(v0.position[0], v0.position[1], v0.position[2], 1.0f);
                        glm::vec4 c1 = mvp * glm::vec4(v1.position[0], v1.position[1], v1.position[2], 1.0f);
                        glm::vec4 c2 = mvp * glm::vec4(v2.position[0], v2.position[1], v2.position[2], 1.0f);

                        if (c0.w < 1e-4f || c1.w < 1e-4f || c2.w < 1e-4f) continue;

                        glm::vec2 ndc[3] = {
                            glm::vec2(c0.x / c0.w, c0.y / c0.w),
                            glm::vec2(c1.x / c1.w, c1.y / c1.w),
                            glm::vec2(c2.x / c2.w, c2.y / c2.w),
                        };

                        // Apply lens distortion per-vertex so the framebuffer
                        // pixel each mesh vertex occupies matches what the
                        // projector lens (which physically distorts the output)
                        // will then re-undistort to land at the world point.
                        // Without this step every rendered vertex is offset by
                        // the lens distortion, and the warp can't fully recover
                        // it because its residuals are computed in distorted
                        // space.
                        if (hasDistortion) {
                            for (int i = 0; i < 3; ++i)
                                ndc[i] = applyDistortionNDC(ndc[i]);
                        }

                        // Apply residual warp by routing each vertex through UV
                        // space, looking up its IDW-blended correction, and
                        // converting back. Shared mesh vertices get the same
                        // warp value so triangle edges stay watertight.
                        if (warpEnabled) {
                            for (int i = 0; i < 3; ++i) {
                                glm::vec2 uv((ndc[i].x + 1.0f) * 0.5f,
                                             (1.0f - ndc[i].y) * 0.5f);
                                uv = applyWarp(uv);
                                ndc[i].x = uv.x * 2.0f - 1.0f;
                                ndc[i].y = 1.0f - uv.y * 2.0f;
                            }
                        }

                        // Backface cull in NDC (CCW front-face → positive area, +Y up).
                        float area = (ndc[1].x - ndc[0].x) * (ndc[2].y - ndc[0].y)
                                   - (ndc[1].y - ndc[0].y) * (ndc[2].x - ndc[0].x);
                        if (area <= 0.0f) continue;

                        glm::vec2 uvs[3] = {
                            glm::vec2(v0.texCoord[0], v0.texCoord[1]),
                            glm::vec2(v1.texCoord[0], v1.texCoord[1]),
                            glm::vec2(v2.texCoord[0], v2.texCoord[1]),
                        };

                        m_renderer->drawMeshTriangle(screenTexture, ndc, uvs);
                    }
                } else {
                    // No mesh: fall back to the default 16:9×1 quad warp
                    // (single draw). Distortion/warp aren't applied to this
                    // path today — meshless screens should be given a mesh
                    // if they need calibration-accurate projection.
                    glm::vec3 worldCorners[4];
                    computeScreenWorldCorners(screen, nullptr, worldCorners);

                    glm::vec2 ndcCorners[4]{};
                    bool allInFront = true;
                    for (int i = 0; i < 4; ++i) {
                        glm::vec4 clip = vp * glm::vec4(worldCorners[i], 1.0f);
                        if (clip.w < 1e-4f) { allInFront = false; break; }
                        ndcCorners[i] = glm::vec2(clip.x / clip.w, clip.y / clip.w);
                    }
                    if (allInFront) {
                        const glm::vec2 uvs[4] = {{0.0f,0.0f},{1.0f,0.0f},{1.0f,1.0f},{0.0f,1.0f}};
                        const glm::vec4 noEdge(0.0f);
                        m_renderer->drawOutputSurface(screenTexture, ndcCorners, uvs, noEdge,
                                                       output.brightness, output.gamma, 1.0f);
                    }
                }
            } // for each target
        }

        // Draw the calibration crosshair overlay directly on top of the warped
        // projector content. Runs on the show command list; the editor thread
        // only updates the state through OutputDisplay::calibrationOverlay.
        // See ADR-0014.
        if (output.calibrationOverlay.enabled) {
            const auto& ov = output.calibrationOverlay;
            m_renderer->drawCalibrationOverlay(
                &ov.points[0][0],
                ov.numPoints,
                ov.activeIndex,
                ov.precisionCursor);
        }

        drawMarkerOverlay(m_renderer);
        m_renderer->endOutputFrame(windowSlot);
        return;
        } // if (projPtr)

        // projPtr not found — fall through to a black frame.
        drawMarkerOverlay(m_renderer);
        m_renderer->endOutputFrame(windowSlot);
        return;
    } // if (output.sourceProjector != 0)

    // Projection-mapping render order (industry-standard model):
    //   compose target -> InputRegion pre-crop -> per-surface warp + source UVs
    //
    // If any OutputSurface is assigned to this output, draw each one with
    // its own corner warp. sourceUVs on the surface are normalized within
    // the output's InputRegion, so we combine them here. If no surfaces are
    // assigned, fall back to a fullscreen InputRegion quad so the output
    // still shows pixels in the setup phase.
    //
    // Corner ordering matches the mapping shader: [0]=TL, [1]=TR, [2]=BR, [3]=BL

    bool drewAny = false;
    for (const auto& surf : surfaces) {
        if (!surf.visible) continue;
        if (surf.outputIndex != output.outputIndex) continue;

        // Combine output's InputRegion pre-crop with surface's own sub-UVs.
        glm::vec2 combinedUVs[4];
        for (int i = 0; i < 4; ++i) {
            combinedUVs[i].x = output.inputRegionX + surf.sourceUVs[i][0] * output.inputRegionWidth;
            combinedUVs[i].y = output.inputRegionY + surf.sourceUVs[i][1] * output.inputRegionHeight;
        }

        glm::vec2 corners[4] = {
            {surf.corners[0][0], surf.corners[0][1]},
            {surf.corners[1][0], surf.corners[1][1]},
            {surf.corners[2][0], surf.corners[2][1]},
            {surf.corners[3][0], surf.corners[3][1]},
        };

        const glm::vec4 softEdges(
            surf.softEdgeLeft, surf.softEdgeRight,
            surf.softEdgeTop,  surf.softEdgeBottom);

        m_renderer->drawOutputSurface(
            compositedTexture,
            corners, combinedUVs,
            softEdges,
            surf.brightness * output.brightness,
            surf.gamma * output.gamma,
            1.0f);
        drewAny = true;
    }

    if (!drewAny) {
        // Fallback: no surfaces attached to this output yet — draw the
        // InputRegion fullscreen so the user isn't staring at a black
        // projector during setup.
        glm::vec2 corners[4] = {
            {-1.0f,  1.0f},
            { 1.0f,  1.0f},
            { 1.0f, -1.0f},
            {-1.0f, -1.0f},
        };
        const float u0 = output.inputRegionX;
        const float v0 = output.inputRegionY;
        const float u1 = u0 + output.inputRegionWidth;
        const float v1 = v0 + output.inputRegionHeight;
        glm::vec2 sourceUVs[4] = {
            {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1},
        };
        const glm::vec4 softEdges(0.0f, 0.0f, 0.0f, 0.0f);
        m_renderer->drawOutputSurface(
            compositedTexture, corners, sourceUVs, softEdges,
            output.brightness, output.gamma, 1.0f);
    }

    drawMarkerOverlay(m_renderer);
    m_renderer->endOutputFrame(windowSlot);
}

} // namespace entity
