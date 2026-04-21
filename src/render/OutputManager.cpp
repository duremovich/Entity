/**
 * OutputManager Implementation
 *
 * Manages output displays for projection mapping, including:
 * - Physical display enumeration (monitors, projectors)
 * - Input region selection (which part of raster feeds each output)
 * - Output rendering coordination
 */

#include "entity/render/OutputManager.hpp"
#include "entity/components/MappingSurface.hpp"
#include "entity/components/Screen.hpp"

#ifdef _WIN32
#include <Windows.h>
#endif

#include <glm/glm.hpp>
#include <iostream>
#include <algorithm>

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

    // Release resources for all outputs. Must happen BEFORE the renderer
    // itself shuts down (it owns the swap chains we reference by slot).
    auto view = m_registry.view<OutputDisplay>();
    for (auto entity : view) {
        releaseOutputResources(entity);
    }

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

    releaseOutputResources(outputEntity);

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

    // If the output already has a window on a different display, tear it
    // down so it can be recreated on the new display.
    if (output.outputWindowSlot != UINT32_MAX) {
        releaseOutputResources(outputEntity);
    }

    output.physicalDisplayIndex = displayIndex;
    output.deviceName = display.deviceName;
    output.displayName = display.displayName;
    output.width = display.width;
    output.height = display.height;
    output.refreshRate = display.refreshRate;
    output.windowX = display.x;
    output.windowY = display.y;

    std::cout << "[OutputManager] Assigned display '" << display.displayName
              << "' to output '" << output.name << "'" << std::endl;

    // Eagerly create the window if the output is already enabled.
    if (output.enabled && output.isPhysical()) {
        ensureOutputWindow(outputEntity);
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

void OutputManager::setOutputEnabled(entt::entity outputEntity, bool enabled) {
    if (!m_registry.valid(outputEntity) || !m_registry.all_of<OutputDisplay>(outputEntity)) {
        return;
    }

    auto& output = m_registry.get<OutputDisplay>(outputEntity);
    output.enabled = enabled;

    // Disabling a Physical output closes its window; enabling triggers lazy
    // creation on the next renderOutputs() (or immediately if we have the
    // display info).
    if (!enabled && output.outputWindowSlot != UINT32_MAX) {
        releaseOutputResources(outputEntity);
    } else if (enabled && output.isPhysical()) {
        ensureOutputWindow(outputEntity);
    }

    std::cout << "[OutputManager] Output '" << output.name << "' "
              << (enabled ? "enabled" : "disabled") << std::endl;
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

void OutputManager::renderOutputs() {
    if (!m_initialized || !m_renderer) {
        return;
    }

    auto view = m_registry.view<OutputDisplay>();
    for (auto [entity, output] : view.each()) {
        if (!output.enabled || !output.isPhysical()) {
            continue;
        }

        // Lazy window creation: if enabled + display assigned but no slot,
        // try to bring it up now. Useful after deserializing a project.
        if (output.outputWindowSlot == UINT32_MAX) {
            ensureOutputWindow(entity);
            if (output.outputWindowSlot == UINT32_MAX) continue;
        }

        TextureRef source = resolveSourceTexture(output);
        if (!source.valid()) {
            // Still clear to black so the physical display isn't showing
            // stale framebuffer content.
            m_renderer->beginOutputFrame(output.outputWindowSlot);
            m_renderer->clearOutputFrame(output.outputWindowSlot, 0.0f, 0.0f, 0.0f, 1.0f);
            m_renderer->endOutputFrame(output.outputWindowSlot);
            continue;
        }

        renderToOutput(entity, source);
    }
}

void OutputManager::createOutputResources(entt::entity outputEntity) {
    // Physical outputs: the window + swap chain live in the renderer and are
    // created lazily via ensureOutputWindow(). Preview / NDI / Virtual types
    // don't own OS-level resources through this path — ImGui handles the
    // preview, and NDI will own its own pipeline later.
}

void OutputManager::releaseOutputResources(entt::entity outputEntity) {
    if (!m_renderer || !m_registry.valid(outputEntity) ||
        !m_registry.all_of<OutputDisplay>(outputEntity)) {
        return;
    }

    auto& output = m_registry.get<OutputDisplay>(outputEntity);
    if (output.outputWindowSlot != UINT32_MAX) {
        m_renderer->destroyOutputWindow(output.outputWindowSlot);
        output.outputWindowSlot = UINT32_MAX;
    }
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

void OutputManager::ensureOutputWindow(entt::entity outputEntity) {
    if (!m_renderer || !m_registry.valid(outputEntity) ||
        !m_registry.all_of<OutputDisplay>(outputEntity)) {
        return;
    }

    auto& output = m_registry.get<OutputDisplay>(outputEntity);
    if (output.outputWindowSlot != UINT32_MAX) return;
    if (!output.isPhysical() || !output.enabled) return;
    if (output.physicalDisplayIndex < 0 ||
        output.physicalDisplayIndex >= static_cast<int32_t>(m_availableDisplays.size())) {
        return;
    }

    const DisplayInfo& display = m_availableDisplays[output.physicalDisplayIndex];
    std::string title = "Entity Output — " + output.name;
    uint32_t slot = m_renderer->createOutputWindow(
        title.c_str(),
        display.x, display.y,
        static_cast<uint32_t>(display.width),
        static_cast<uint32_t>(display.height));

    if (slot == UINT32_MAX) {
        std::cerr << "[OutputManager] Failed to create output window for '"
                  << output.name << "'" << std::endl;
        return;
    }

    output.outputWindowSlot = slot;
    output.width = display.width;
    output.height = display.height;
    std::cout << "[OutputManager] Output '" << output.name
              << "' driving display '" << display.displayName
              << "' (slot " << slot << ")" << std::endl;
}

TextureRef OutputManager::resolveSourceTexture(const OutputDisplay& output) const {
    // Explicit route: use the Screen the user assigned (if valid).
    if (output.sourceScreen != entt::null &&
        m_registry.valid(output.sourceScreen) &&
        m_registry.all_of<Screen>(output.sourceScreen)) {
        const auto& s = m_registry.get<Screen>(output.sourceScreen);
        if (s.renderTargetValid && s.renderTargetSlot != UINT32_MAX) {
            return m_renderer->getComposeTargetTexture(s.renderTargetSlot);
        }
    }

    // Fallback: first visible Screen with a live compose target.
    auto view = m_registry.view<Screen>();
    for (auto [entity, screen] : view.each()) {
        if (!screen.visible) continue;
        if (!screen.renderTargetValid || screen.renderTargetSlot == UINT32_MAX) continue;
        return m_renderer->getComposeTargetTexture(screen.renderTargetSlot);
    }

    return TextureRef::invalid();
}

void OutputManager::renderToOutput(entt::entity outputEntity, TextureRef compositedTexture) {
    if (!m_renderer || !m_registry.valid(outputEntity)) {
        return;
    }

    auto& output = m_registry.get<OutputDisplay>(outputEntity);
    if (output.outputWindowSlot == UINT32_MAX) return;
    if (!compositedTexture.valid()) return;

    m_renderer->beginOutputFrame(output.outputWindowSlot);
    m_renderer->clearOutputFrame(output.outputWindowSlot, 0.0f, 0.0f, 0.0f, 1.0f);

    // Projection-mapping render order (Disguise model):
    //   compose target -> InputRegion pre-crop -> per-surface warp + source UVs
    //
    // If any MappingSurface is assigned to this output, draw each one with
    // its own corner warp. sourceUVs on the surface are normalized within
    // the output's InputRegion, so we combine them here. If no surfaces are
    // assigned, fall back to a fullscreen InputRegion quad so the output
    // still shows pixels in the setup phase.
    //
    // Corner ordering matches the mapping shader: [0]=TL, [1]=TR, [2]=BR, [3]=BL

    auto surfView = m_registry.view<MappingSurface>();
    bool drewAny = false;
    for (auto [surfEntity, surf] : surfView.each()) {
        if (!surf.visible) continue;
        if (surf.outputIndex != output.outputIndex) continue;

        // Combine output's InputRegion pre-crop with surface's own sub-UVs.
        glm::vec2 combinedUVs[4];
        for (int i = 0; i < 4; ++i) {
            combinedUVs[i].x = output.inputRegion.x + surf.sourceUVs[i].x * output.inputRegion.width;
            combinedUVs[i].y = output.inputRegion.y + surf.sourceUVs[i].y * output.inputRegion.height;
        }

        glm::vec2 corners[4] = {
            surf.corners[0],
            surf.corners[1],
            surf.corners[2],
            surf.corners[3],
        };

        const glm::vec4 softEdges(
            surf.softEdge.left, surf.softEdge.right,
            surf.softEdge.top,  surf.softEdge.bottom);

        m_renderer->drawMappingSurface(
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
        const float u0 = output.inputRegion.x;
        const float v0 = output.inputRegion.y;
        const float u1 = u0 + output.inputRegion.width;
        const float v1 = v0 + output.inputRegion.height;
        glm::vec2 sourceUVs[4] = {
            {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1},
        };
        const glm::vec4 softEdges(0.0f, 0.0f, 0.0f, 0.0f);
        m_renderer->drawMappingSurface(
            compositedTexture, corners, sourceUVs, softEdges,
            output.brightness, output.gamma, 1.0f);
    }

    m_renderer->endOutputFrame(output.outputWindowSlot);
}

} // namespace entity
