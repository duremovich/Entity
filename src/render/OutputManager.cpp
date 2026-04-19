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

#ifdef _WIN32
#include <Windows.h>
#endif

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

    // Release resources for all outputs
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

void OutputManager::renderOutputs(TextureRef compositedTexture) {
    if (!m_initialized || !compositedTexture.valid()) {
        return;
    }

    // Render to each enabled output
    auto view = m_registry.view<OutputDisplay>();
    for (auto [entity, output] : view.each()) {
        if (output.enabled) {
            renderToOutput(entity, compositedTexture);
        }
    }
}

void OutputManager::createOutputResources(entt::entity outputEntity) {
    // TODO: Create render target for this output
    // For physical outputs, this would be a swap chain on the target display
    // For preview, it's a texture we can display in ImGui
    // For NDI, it's a staging buffer for CPU readback
}

void OutputManager::releaseOutputResources(entt::entity outputEntity) {
    // TODO: Release render resources for this output
}

void OutputManager::renderToOutput(entt::entity outputEntity, TextureRef compositedTexture) {
    if (!m_renderer || !m_registry.valid(outputEntity)) {
        return;
    }

    auto& output = m_registry.get<OutputDisplay>(outputEntity);

    // For now, the main window acts as the preview output
    // Full implementation would:
    // 1. Set render target to this output's swap chain/texture
    // 2. Clear the target
    // 3. Sample the composited texture using the input region UVs
    // 4. Render through any mapping surfaces assigned to this output
    // 5. Present or copy to output

    // The CompositorSystem already handles rendering to the main window
    // This infrastructure is for when we have multiple output windows
}

} // namespace entity
