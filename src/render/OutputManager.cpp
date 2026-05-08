/**
 * OutputManager Implementation
 *
 * Manages output displays for projection mapping, including:
 * - Physical display enumeration (monitors, projectors)
 * - Input region selection (which part of raster feeds each output)
 * - Output rendering coordination
 */

#include "entity/render/OutputManager.hpp"
#include "entity/bus/Message.hpp"
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

void OutputManager::renderOutputs(const bus::RenderFrame& rf) {
    if (!m_initialized || !m_renderer) {
        return;
    }

    for (const auto& snap : rf.outputs) {
        if (!snap.enabled || !snap.isPhysical) {
            continue;
        }

        const entt::entity entity = static_cast<entt::entity>(snap.entity);

        // Lazy window creation: if enabled + display assigned but no slot,
        // try to bring it up now. Useful after deserializing a project.
        uint32_t windowSlot = snap.outputWindowSlot;
        if (windowSlot == UINT32_MAX) {
            windowSlot = ensureOutputWindow(entity);
            if (windowSlot == UINT32_MAX) continue;
        }

        TextureRef source = resolveSourceTexture(snap, rf.screens, rf.projectors);
        if (!source.valid()) {
            m_renderer->beginOutputFrame(windowSlot);
            m_renderer->clearOutputFrame(windowSlot, 0.0f, 0.0f, 0.0f, 1.0f);
            m_renderer->endOutputFrame(windowSlot);
            continue;
        }

        renderToOutput(snap, windowSlot, source, rf.surfaces, rf.projectors, rf.screens);
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

uint32_t OutputManager::ensureOutputWindow(entt::entity outputEntity) {
    if (!m_renderer || !m_registry.valid(outputEntity) ||
        !m_registry.all_of<OutputDisplay>(outputEntity)) {
        return UINT32_MAX;
    }

    auto& output = m_registry.get<OutputDisplay>(outputEntity);
    if (output.outputWindowSlot != UINT32_MAX) return output.outputWindowSlot;
    if (!output.isPhysical() || !output.enabled) return UINT32_MAX;
    if (output.physicalDisplayIndex < 0 ||
        output.physicalDisplayIndex >= static_cast<int32_t>(m_availableDisplays.size())) {
        return UINT32_MAX;
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
        return UINT32_MAX;
    }

    output.outputWindowSlot = slot;
    output.width = display.width;
    output.height = display.height;
    std::cout << "[OutputManager] Output '" << output.name
              << "' driving display '" << display.displayName
              << "' (slot " << slot << ")" << std::endl;
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
// Order: [0]=TL, [1]=TR, [2]=BR, [3]=BL (drawMappingSurface convention).
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

    // Projector source: find the projector's target screen.
    if (output.sourceProjector != 0) {
        for (const auto& proj : projectors) {
            if (proj.entity != output.sourceProjector) continue;
            // Use first target surface if set.
            if (proj.targetSurfaceCount > 0 && proj.targetSurfaces[0] != 0) {
                if (const auto* s = findScreenByEntity(proj.targetSurfaces[0]))
                    return textureForScreen(*s);
            }
            // Fallback: first visible screen.
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
        const std::vector<bus::MappingSurfaceSnapshot>& surfaces,
        const std::vector<bus::ProjectorSnapshot>& projectors,
        const std::vector<bus::ScreenSnapshot>& screens) {
    if (!m_renderer || !compositedTexture.valid()) return;

    m_renderer->beginOutputFrame(windowSlot);
    m_renderer->clearOutputFrame(windowSlot, 0.0f, 0.0f, 0.0f, 1.0f);

    // Projector-linked output: render the screen content as seen from the projector's
    // camera. Project the screen's 3D corners through the projector VP to get NDC
    // positions, then use drawMappingSurface for perspective-correct warping.
    if (output.sourceProjector != 0) {
        // Find projector snapshot
        const bus::ProjectorSnapshot* projPtr = nullptr;
        for (const auto& p : projectors)
            if (p.entity == output.sourceProjector) { projPtr = &p; break; }

        if (projPtr) {
        const auto& proj = *projPtr;

        // Find the target screen snapshot (same resolution logic as resolveSourceTexture).
        const bus::ScreenSnapshot* screenSnap = nullptr;
        if (proj.targetSurfaceCount > 0 && proj.targetSurfaces[0] != 0) {
            for (const auto& s : screens)
                if (s.entity == proj.targetSurfaces[0]) { screenSnap = &s; break; }
        }
        if (!screenSnap) {
            for (const auto& s : screens)
                if (s.visible) { screenSnap = &s; break; }
        }

        if (screenSnap) {
            const auto& screen = *screenSnap;

            // Projector mesh-render pipeline: pose → per-vertex k1/k2 lens
            // distortion → optional IDW residual warp → triangle. The
            // distortion step is load-bearing — see ADR-0011 for the
            // framebuffer↔world chain. Skipping it here makes every
            // vertex land off by the lens distortion amount even before
            // the warp runs, which manifests as "warp doesn't quite hit
            // the cal points".

            // Resolve mesh — when present, render per-triangle so the projector
            // output matches what the calibration window's right pane shows.
            // Mesh data stays in the registry (read-only, read-mostly).
            // Stage 4 will move this to a renderer-side mesh store via
            // bus::ProvisionMesh (open question #1 in the plan).
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

            float aspect = output.height > 0 ? static_cast<float>(output.width) / output.height
                                              : 16.0f / 9.0f;
            glm::mat4 vp = buildProjectorVP(proj, aspect);

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

                // ============================================================
                // Optional: post-fit residual warp via inverse-distance
                // weighting of calibration-point residuals.
                //
                // For each calibration point, compute (predicted UV via the
                // calibrated pose+distortion, residual to user's measured UV).
                // For every mesh-triangle vertex's projected UV, blend the
                // residuals weighted by 1/distance². The result is a smooth
                // 2D warp that lands every calibration point EXACTLY on its
                // measured UV, with the surface in between flowing smoothly.
                //
                // This is the standard "pose calibration → residual warp"
                // approach used by disguise/MadMapper for the last-mile gap.
                // ============================================================
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

                    m_renderer->drawMeshTriangle(compositedTexture, ndc, uvs);
                }
            } else {
                // No mesh: fall back to the default 16:9×1 quad warp (single draw).
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
                    m_renderer->drawMappingSurface(compositedTexture, ndcCorners, uvs, noEdge,
                                                   output.brightness, output.gamma, 1.0f);
                }
            }
        }

        // Composite the calibration crosshair overlay on top, if active.
        if (output.calibrationOverlaySlot != UINT32_MAX) {
            TextureRef overlayTex = m_renderer->getComposeTargetTexture(output.calibrationOverlaySlot);
            if (overlayTex.valid()) {
                const glm::vec2 fsCorners[4] = {{-1.0f,1.0f},{1.0f,1.0f},{1.0f,-1.0f},{-1.0f,-1.0f}};
                const glm::vec2 fsUVs[4]     = {{0.0f,0.0f},{1.0f,0.0f},{1.0f,1.0f},{0.0f,1.0f}};
                m_renderer->drawMappingSurface(overlayTex, fsCorners, fsUVs, glm::vec4(0.0f),
                                               1.0f, 1.0f, 1.0f);
            }
        }

        m_renderer->endOutputFrame(windowSlot);
        return;
        } // if (projPtr)

        // projPtr not found — fall through to a black frame.
        m_renderer->endOutputFrame(windowSlot);
        return;
    } // if (output.sourceProjector != 0)

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
        const float u0 = output.inputRegionX;
        const float v0 = output.inputRegionY;
        const float u1 = u0 + output.inputRegionWidth;
        const float v1 = v0 + output.inputRegionHeight;
        glm::vec2 sourceUVs[4] = {
            {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1},
        };
        const glm::vec4 softEdges(0.0f, 0.0f, 0.0f, 0.0f);
        m_renderer->drawMappingSurface(
            compositedTexture, corners, sourceUVs, softEdges,
            output.brightness, output.gamma, 1.0f);
    }

    m_renderer->endOutputFrame(windowSlot);
}

} // namespace entity
