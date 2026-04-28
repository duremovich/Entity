#include "entity/ui/MappingWindow.hpp"
#include "entity/color/OcioManager.hpp"
#include "entity/core/Engine.hpp"
#include "entity/components/Screen.hpp"
#include "entity/render/D3D12Renderer.hpp"
#include "entity/render/OutputManager.hpp"
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace entity {

MappingWindow::MappingWindow(Engine* engine)
    : m_engine(engine)
{
}

void MappingWindow::render() {
    if (!m_engine) return;

    ImVec2 windowSize = ImGui::GetContentRegionAvail();

    // Toolbar
    renderToolbar();

    ImGui::Separator();

    // Calculate panel sizes
    float outputsPanelWidth = m_showOutputsPanel ? 250.0f : 0.0f;
    float propertiesHeight = 150.0f;
    float canvasWidth = windowSize.x - outputsPanelWidth - (m_showOutputsPanel ? 8.0f : 0.0f);
    float canvasHeight = windowSize.y - propertiesHeight - 60.0f;

    // Horizontal layout: outputs panel on left, canvas on right
    if (m_showOutputsPanel) {
        ImGui::BeginChild("OutputsPanel", ImVec2(outputsPanelWidth, canvasHeight + propertiesHeight), true);
        renderOutputsPanel();
        ImGui::EndChild();

        ImGui::SameLine();
    }

    // Right side: canvas + properties stacked vertically
    ImGui::BeginGroup();

    // Main canvas area
    if (canvasHeight > 100.0f) {
        ImGui::BeginChild("MappingCanvas", ImVec2(canvasWidth, canvasHeight), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        renderCanvas();
        ImGui::EndChild();
    }

    ImGui::Separator();

    // Properties panel at bottom
    ImGui::BeginChild("MappingProperties", ImVec2(canvasWidth, propertiesHeight), true);
    renderPropertiesPanel();
    ImGui::EndChild();

    ImGui::EndGroup();
}

void MappingWindow::renderToolbar() {
    ImGui::BeginGroup();

    // Add surface button
    if (ImGui::Button("Add Surface")) {
        // Find highest existing surface number
        int maxSurfaceNum = 0;
        auto& registry = m_engine->getRegistry();
        auto surfaceView = registry.view<MappingSurface>();
        for (auto [entity, surface] : surfaceView.each()) {
            if (surface.name.find("Surface ") == 0) {
                try {
                    int num = std::stoi(surface.name.substr(8));
                    maxSurfaceNum = std::max(maxSurfaceNum, num);
                } catch (...) {
                    // Ignore surfaces with non-numeric suffixes
                }
            }
        }
        std::string name = "Surface " + std::to_string(maxSurfaceNum + 1);
        m_selectedSurface = createSurface(name);
    }

    ImGui::SameLine();

    // Delete surface button
    ImGui::BeginDisabled(m_selectedSurface == entt::null);
    if (ImGui::Button("Delete Surface")) {
        deleteSelectedSurface();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Spacing();
    ImGui::SameLine();

    // View toggles
    ImGui::Checkbox("Grid", &m_showGrid);
    ImGui::SameLine();
    ImGui::Checkbox("Handles", &m_showHandles);
    ImGui::SameLine();
    ImGui::Checkbox("Soft Edge Preview", &m_showSoftEdge);
    ImGui::SameLine();
    ImGui::Checkbox("Outputs", &m_showOutputsPanel);

    ImGui::EndGroup();
}

void MappingWindow::renderCanvas() {
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();

    // Create canvas background
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Dark background
    drawList->AddRectFilled(canvasPos,
                            ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                            IM_COL32(30, 30, 35, 255));

    // Draw grid if enabled
    if (m_showGrid) {
        float gridSpacing = 50.0f;
        for (float x = 0; x < canvasSize.x; x += gridSpacing) {
            drawList->AddLine(
                ImVec2(canvasPos.x + x, canvasPos.y),
                ImVec2(canvasPos.x + x, canvasPos.y + canvasSize.y),
                COLOR_GRID
            );
        }
        for (float y = 0; y < canvasSize.y; y += gridSpacing) {
            drawList->AddLine(
                ImVec2(canvasPos.x, canvasPos.y + y),
                ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + y),
                COLOR_GRID
            );
        }
    }

    // Draw center crosshair
    ImVec2 center(canvasPos.x + canvasSize.x * 0.5f, canvasPos.y + canvasSize.y * 0.5f);
    drawList->AddLine(ImVec2(center.x - 20, center.y), ImVec2(center.x + 20, center.y), IM_COL32(100, 100, 100, 150));
    drawList->AddLine(ImVec2(center.x, center.y - 20), ImVec2(center.x, center.y + 20), IM_COL32(100, 100, 100, 150));

    // Render all mapping surfaces
    auto& registry = m_engine->getRegistry();
    auto view = registry.view<MappingSurface>();

    for (auto [entity, surface] : view.each()) {
        renderSurface(entity, surface, drawList, canvasPos, canvasSize);
    }

    // Handle mouse interaction
    handleInteraction(canvasPos, canvasSize);
}

void MappingWindow::renderSurface(entt::entity entity, MappingSurface& surface,
                                   ImDrawList* drawList, const ImVec2& canvasPos,
                                   const ImVec2& canvasSize) {
    if (!surface.visible) return;

    bool isSelected = (entity == m_selectedSurface);
    ImU32 outlineColor = isSelected ? COLOR_SURFACE_SELECTED : COLOR_SURFACE_OUTLINE;

    // Convert corners to canvas positions
    ImVec2 corners[4];
    for (int i = 0; i < 4; ++i) {
        corners[i] = normalizedToCanvas(surface.corners[i], canvasPos, canvasSize);
    }

    // Draw filled quad
    drawList->AddQuadFilled(corners[0], corners[1], corners[2], corners[3], COLOR_SURFACE_FILL);

    // Draw outline
    drawList->AddQuad(corners[0], corners[1], corners[2], corners[3], outlineColor, 2.0f);

    // Draw diagonals for visual reference
    drawList->AddLine(corners[0], corners[2], IM_COL32(80, 80, 80, 100), 1.0f);
    drawList->AddLine(corners[1], corners[3], IM_COL32(80, 80, 80, 100), 1.0f);

    // Draw corner handles if selected and handles enabled
    if (isSelected && m_showHandles) {
        renderCornerHandles(surface, drawList, canvasPos, canvasSize);
    }

    // Draw surface name
    ImVec2 center(
        (corners[0].x + corners[1].x + corners[2].x + corners[3].x) * 0.25f,
        (corners[0].y + corners[1].y + corners[2].y + corners[3].y) * 0.25f
    );
    ImVec2 textSize = ImGui::CalcTextSize(surface.name.c_str());
    drawList->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f),
                      IM_COL32(255, 255, 255, 200), surface.name.c_str());
}

void MappingWindow::renderCornerHandles(MappingSurface& surface, ImDrawList* drawList,
                                         const ImVec2& canvasPos, const ImVec2& canvasSize) {
    const char* cornerLabels[] = {"TL", "TR", "BR", "BL"};

    for (int i = 0; i < 4; ++i) {
        ImVec2 pos = normalizedToCanvas(surface.corners[i], canvasPos, canvasSize);
        bool isActive = (m_draggingCorner == i);
        ImU32 color = isActive ? COLOR_CORNER_ACTIVE : COLOR_CORNER_HANDLE;

        // Draw handle circle
        drawList->AddCircleFilled(pos, CORNER_HANDLE_SIZE, color);
        drawList->AddCircle(pos, CORNER_HANDLE_SIZE, IM_COL32(0, 0, 0, 200), 0, 2.0f);

        // Draw corner label
        ImVec2 labelOffset(10.0f, -15.0f);
        if (i == 1 || i == 2) labelOffset.x = -20.0f;  // Right side labels offset left
        if (i == 2 || i == 3) labelOffset.y = 5.0f;     // Bottom labels offset down

        drawList->AddText(ImVec2(pos.x + labelOffset.x, pos.y + labelOffset.y),
                          IM_COL32(200, 200, 200, 200), cornerLabels[i]);
    }
}

void MappingWindow::renderPropertiesPanel() {
    if (m_selectedSurface == entt::null) {
        ImGui::TextDisabled("No surface selected");
        ImGui::TextDisabled("Click 'Add Surface' to create one");
        return;
    }

    auto& registry = m_engine->getRegistry();
    auto* surface = registry.try_get<MappingSurface>(m_selectedSurface);
    if (!surface) {
        m_selectedSurface = entt::null;
        return;
    }

    ImGui::Text("Surface: %s", surface->name.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 100);
    ImGui::Checkbox("Visible", &surface->visible);

    // Output assignment — surfaces are drawn on the output whose outputIndex matches.
    {
        const char* preview = "(unassigned)";
        std::string label;
        auto outView = registry.view<OutputDisplay>();
        entt::entity assigned = entt::null;
        for (auto [outEntity, out] : outView.each()) {
            if (out.outputIndex == surface->outputIndex) {
                label = out.name + " [" + out.getTypeString() + "]";
                preview = label.c_str();
                assigned = outEntity;
                break;
            }
        }
        if (ImGui::BeginCombo("Output##surfAssign", preview)) {
            for (auto [outEntity, out] : outView.each()) {
                char item[256];
                std::snprintf(item, sizeof(item), "%s [%s]",
                              out.name.c_str(), out.getTypeString());
                bool sel = (outEntity == assigned);
                ImGui::PushID(static_cast<int>(outEntity));
                if (ImGui::Selectable(item, sel)) {
                    surface->outputIndex = out.outputIndex;
                }
                if (sel) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // Two columns: corners on left, properties on right
    ImGui::Columns(2, "SurfaceProperties", true);

    // Corner positions
    ImGui::Text("Corner Positions");
    const char* cornerNames[] = {"Top-Left", "Top-Right", "Bottom-Right", "Bottom-Left"};
    for (int i = 0; i < 4; ++i) {
        ImGui::PushID(i);
        ImGui::Text("%s:", cornerNames[i]);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat2("##corner", &surface->corners[i].x, 0.01f, -2.0f, 2.0f);
        ImGui::PopID();
    }

    if (ImGui::Button("Reset Corners")) {
        surface->resetCorners();
    }

    // Sizing presets — reshape the surface corners to standard fit modes.
    // Requires knowing source (compose target = sourceScreen of the
    // assigned output) and output (physical display) dimensions.
    int sourceW = 0, sourceH = 0;
    int outputW = 0, outputH = 0;
    {
        OutputDisplay* assigned = nullptr;
        auto outView = registry.view<OutputDisplay>();
        for (auto [oe, out] : outView.each()) {
            if (out.outputIndex == surface->outputIndex) {
                assigned = &out;
                break;
            }
        }
        if (assigned) {
            outputW = assigned->width;
            outputH = assigned->height;
            entt::entity src = assigned->sourceScreen;
            if (src == entt::null) {
                for (auto [se, s] : registry.view<Screen>().each()) {
                    if (s.visible) { src = se; break; }
                }
            }
            if (registry.valid(src) && registry.all_of<Screen>(src)) {
                const auto& s = registry.get<Screen>(src);
                sourceW = static_cast<int>(s.width);
                sourceH = static_cast<int>(s.height);
            }
        }
    }

    auto setCorners = [&](float halfX, float halfY) {
        surface->corners = {{
            {-halfX,  halfY}, { halfX,  halfY},
            { halfX, -halfY}, {-halfX, -halfY}
        }};
    };

    if (outputW > 0 && outputH > 0 && sourceW > 0 && sourceH > 0) {
        const float srcAR = float(sourceW) / float(sourceH);
        const float outAR = float(outputW) / float(outputH);

        ImGui::Text("Sizing Presets");
        if (ImGui::Button("Stretch")) {
            setCorners(1.0f, 1.0f);  // fullscreen, ignores aspect
        }
        ImGui::SameLine();
        if (ImGui::Button("Fit")) {
            // Letterbox/pillarbox inside output preserving source aspect.
            if (srcAR > outAR) setCorners(1.0f, outAR / srcAR);    // letterbox
            else               setCorners(srcAR / outAR, 1.0f);    // pillarbox
        }
        ImGui::SameLine();
        if (ImGui::Button("Fit W")) {
            setCorners(1.0f, outAR / srcAR);
        }
        ImGui::SameLine();
        if (ImGui::Button("Fit H")) {
            setCorners(srcAR / outAR, 1.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button("1:1")) {
            // Pixel-perfect — source pixels land 1:1 on output pixels.
            setCorners(float(sourceW) / float(outputW),
                       float(sourceH) / float(outputH));
        }
        ImGui::TextDisabled("src %dx%d  out %dx%d", sourceW, sourceH, outputW, outputH);
    } else {
        ImGui::TextDisabled("(assign output + source screen for presets)");
    }

    ImGui::NextColumn();

    // Surface properties
    ImGui::Text("Properties");
    ImGui::SliderFloat("Brightness", &surface->brightness, 0.0f, 2.0f);
    ImGui::SliderFloat("Gamma", &surface->gamma, 0.5f, 2.5f);
    ImGui::SliderInt("Grid Subdivisions", reinterpret_cast<int*>(&surface->gridSubdivisions), 1, 10);

    ImGui::Spacing();
    ImGui::Text("Soft Edges (Blend)");
    ImGui::SliderFloat("Left##edge", &surface->softEdge.left, 0.0f, 0.5f);
    ImGui::SliderFloat("Right##edge", &surface->softEdge.right, 0.0f, 0.5f);
    ImGui::SliderFloat("Top##edge", &surface->softEdge.top, 0.0f, 0.5f);
    ImGui::SliderFloat("Bottom##edge", &surface->softEdge.bottom, 0.0f, 0.5f);

    ImGui::Columns(1);
}

void MappingWindow::handleInteraction(const ImVec2& canvasPos, const ImVec2& canvasSize) {
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 mousePos = io.MousePos;

    // Check if mouse is in canvas area
    bool inCanvas = mousePos.x >= canvasPos.x && mousePos.x < canvasPos.x + canvasSize.x &&
                    mousePos.y >= canvasPos.y && mousePos.y < canvasPos.y + canvasSize.y;

    if (!inCanvas) {
        return;
    }

    auto& registry = m_engine->getRegistry();
    glm::vec2 normalizedMouse = canvasToNormalized(mousePos, canvasPos, canvasSize);

    // Handle dragging
    if (m_draggingCorner >= 0 && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (m_selectedSurface != entt::null) {
            auto* surface = registry.try_get<MappingSurface>(m_selectedSurface);
            if (surface) {
                surface->corners[m_draggingCorner] = normalizedMouse;
            }
        }
    }

    // Handle surface dragging
    if (m_isDraggingSurface && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (m_selectedSurface != entt::null) {
            auto* surface = registry.try_get<MappingSurface>(m_selectedSurface);
            if (surface) {
                glm::vec2 newCenter = normalizedMouse - m_dragOffset;
                glm::vec2 currentCenter = surface->getCenter();
                surface->translate(newCenter - currentCenter);
            }
        }
    }

    // Handle mouse release
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        m_draggingCorner = -1;
        m_isDraggingSurface = false;
    }

    // Handle mouse click
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool foundCorner = false;

        // First check if clicking a corner handle of selected surface
        if (m_selectedSurface != entt::null && m_showHandles) {
            auto* surface = registry.try_get<MappingSurface>(m_selectedSurface);
            if (surface) {
                for (int i = 0; i < 4; ++i) {
                    ImVec2 cornerPos = normalizedToCanvas(surface->corners[i], canvasPos, canvasSize);
                    float dist = std::sqrt(std::pow(mousePos.x - cornerPos.x, 2) +
                                           std::pow(mousePos.y - cornerPos.y, 2));
                    if (dist < CORNER_HIT_SIZE) {
                        m_draggingCorner = i;
                        foundCorner = true;
                        break;
                    }
                }
            }
        }

        // If not clicking a corner, check for surface selection
        if (!foundCorner) {
            entt::entity clickedSurface = entt::null;
            auto view = registry.view<MappingSurface>();

            for (auto [entity, surface] : view.each()) {
                if (surface.visible && surface.containsPoint(normalizedMouse)) {
                    clickedSurface = entity;
                    // Don't break - later surfaces are rendered on top
                }
            }

            if (clickedSurface != entt::null) {
                // Check if we're clicking the already selected surface
                if (clickedSurface == m_selectedSurface) {
                    // Start dragging the whole surface
                    auto* surface = registry.try_get<MappingSurface>(clickedSurface);
                    if (surface) {
                        m_isDraggingSurface = true;
                        m_dragOffset = normalizedMouse - surface->getCenter();
                    }
                } else {
                    // Select new surface
                    m_selectedSurface = clickedSurface;
                }
            } else {
                // Clicked empty space - deselect
                m_selectedSurface = entt::null;
            }
        }
    }
}

ImVec2 MappingWindow::normalizedToCanvas(const glm::vec2& pos, const ImVec2& canvasPos,
                                          const ImVec2& canvasSize) const {
    // Map from [-1, 1] to canvas pixel coordinates
    // Flip Y because ImGui Y is down, normalized Y is up
    float x = canvasPos.x + (pos.x + 1.0f) * 0.5f * canvasSize.x;
    float y = canvasPos.y + (1.0f - pos.y) * 0.5f * canvasSize.y;
    return ImVec2(x, y);
}

glm::vec2 MappingWindow::canvasToNormalized(const ImVec2& pos, const ImVec2& canvasPos,
                                             const ImVec2& canvasSize) const {
    // Map from canvas pixel coordinates to [-1, 1]
    float x = ((pos.x - canvasPos.x) / canvasSize.x) * 2.0f - 1.0f;
    float y = 1.0f - ((pos.y - canvasPos.y) / canvasSize.y) * 2.0f;
    return glm::vec2(x, y);
}

entt::entity MappingWindow::createSurface(const std::string& name) {
    auto& registry = m_engine->getRegistry();

    entt::entity entity = registry.create();
    auto& surface = registry.emplace<MappingSurface>(entity);
    surface.name = name;

    // Count existing surfaces for index
    auto view = registry.view<MappingSurface>();
    surface.surfaceIndex = static_cast<uint32_t>(std::distance(view.begin(), view.end())) - 1;

    // Auto-associate with the currently-selected output so dragging corners
    // immediately affects the projection you're looking at. Falls back to
    // output 0 if no output is selected (matches the component default).
    if (m_selectedOutput != entt::null && registry.valid(m_selectedOutput)) {
        auto* out = registry.try_get<OutputDisplay>(m_selectedOutput);
        if (out) {
            surface.outputIndex = out->outputIndex;
            std::cout << "[MappingWindow] Surface '" << name << "' assigned to output '"
                      << out->name << "' (index " << out->outputIndex << ")" << std::endl;
        }
    } else {
        std::cout << "[MappingWindow] Surface '" << name
                  << "' has no selected output — defaulting to output 0" << std::endl;
    }

    std::cout << "[MappingWindow] Created surface: " << name << " (entity=" << static_cast<uint32_t>(entity) << ")" << std::endl;

    return entity;
}

void MappingWindow::deleteSelectedSurface() {
    if (m_selectedSurface == entt::null) return;

    auto& registry = m_engine->getRegistry();

    if (registry.valid(m_selectedSurface)) {
        auto* surface = registry.try_get<MappingSurface>(m_selectedSurface);
        if (surface) {
            std::cout << "[MappingWindow] Deleted surface: " << surface->name << std::endl;
        }
        registry.destroy(m_selectedSurface);
    }

    m_selectedSurface = entt::null;
    m_draggingCorner = -1;
}

void MappingWindow::renderOutputsPanel() {
    auto& registry = m_engine->getRegistry();

    // Header
    ImGui::Text("Outputs");
    ImGui::Separator();

    // Add output buttons
    if (ImGui::Button("+ Physical")) {
        // Find highest existing output number
        int maxOutputNum = 0;
        auto outputView = registry.view<OutputDisplay>();
        for (auto [entity, existingOutput] : outputView.each()) {
            if (existingOutput.name.find("Output ") == 0) {
                try {
                    int num = std::stoi(existingOutput.name.substr(7));
                    maxOutputNum = std::max(maxOutputNum, num);
                } catch (...) {
                    // Ignore outputs with non-numeric suffixes
                }
            }
        }

        entt::entity newOutput = registry.create();
        auto& output = registry.emplace<OutputDisplay>(newOutput);
        output.name = "Output " + std::to_string(maxOutputNum + 1);
        output.type = OutputType::Physical;
        // Start disabled with no display — user explicitly picks display
        // then ticks Enabled. Prevents "+ Physical" from immediately
        // blacking out the primary display.
        output.enabled = false;
        output.physicalDisplayIndex = -1;
        m_selectedOutput = newOutput;

        // Auto-create a fullscreen mapping surface for this output so
        // enabling it immediately shows the composited raster. User can
        // drag corners or hit a preset button without first clicking
        // "Add Surface".
        int maxSurfaceNum = 0;
        auto surfCountView = registry.view<MappingSurface>();
        for (auto [se, existingSurf] : surfCountView.each()) {
            if (existingSurf.name.find("Surface ") == 0) {
                try {
                    maxSurfaceNum = std::max(maxSurfaceNum,
                        std::stoi(existingSurf.name.substr(8)));
                } catch (...) {}
            }
        }
        std::string surfName = "Surface " + std::to_string(maxSurfaceNum + 1);
        entt::entity newSurf = createSurface(surfName);
        if (auto* s = registry.try_get<MappingSurface>(newSurf)) {
            s->corners = {{
                {-1.0f,  1.0f}, { 1.0f,  1.0f},
                { 1.0f, -1.0f}, {-1.0f, -1.0f}
            }};
        }
        m_selectedSurface = newSurf;

        std::cout << "[MappingWindow] Created physical output: " << output.name
                  << " (disabled, no display assigned, 1 fullscreen surface)" << std::endl;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Preview")) {
        // Find highest existing preview number
        int maxPreviewNum = 0;
        auto outputView = registry.view<OutputDisplay>();
        for (auto [entity, existingOutput] : outputView.each()) {
            if (existingOutput.name.find("Preview ") == 0) {
                try {
                    int num = std::stoi(existingOutput.name.substr(8));
                    maxPreviewNum = std::max(maxPreviewNum, num);
                } catch (...) {
                    // Ignore previews with non-numeric suffixes
                }
            }
        }

        entt::entity newOutput = registry.create();
        auto& output = registry.emplace<OutputDisplay>(newOutput);
        output.name = "Preview " + std::to_string(maxPreviewNum + 1);
        output.type = OutputType::Preview;
        output.width = 1280;
        output.height = 720;
        m_selectedOutput = newOutput;
        std::cout << "[MappingWindow] Created preview output: " << output.name << std::endl;
    }

    ImGui::Spacing();

    // List of outputs
    auto view = registry.view<OutputDisplay>();
    for (auto [entity, output] : view.each()) {
        ImGui::PushID(static_cast<int>(entity));

        bool isSelected = (entity == m_selectedOutput);
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        if (isSelected) flags |= ImGuiTreeNodeFlags_Selected;

        // Icon based on type
        const char* icon = output.type == OutputType::Physical ? "[P]" :
                           output.type == OutputType::Preview ? "[V]" :
                           output.type == OutputType::NDI ? "[N]" : "[?]";

        ImGui::TreeNodeEx(reinterpret_cast<void*>(static_cast<uintptr_t>(entity)), flags, "%s %s", icon, output.name.c_str());

        if (ImGui::IsItemClicked()) {
            m_selectedOutput = entity;
        }

        // Context menu for output
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Delete")) {
                registry.destroy(entity);
                if (m_selectedOutput == entity) {
                    m_selectedOutput = entt::null;
                }
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
    }

    ImGui::Separator();

    // Selected output properties
    if (m_selectedOutput != entt::null && registry.valid(m_selectedOutput)) {
        auto* output = registry.try_get<OutputDisplay>(m_selectedOutput);
        if (output) {
            ImGui::Text("Output Properties");
            ImGui::Spacing();

            // Name
            char nameBuffer[128];
            strncpy(nameBuffer, output->name.c_str(), sizeof(nameBuffer) - 1);
            nameBuffer[sizeof(nameBuffer) - 1] = '\0';
            if (ImGui::InputText("Name", nameBuffer, sizeof(nameBuffer))) {
                output->name = nameBuffer;
            }

            // Type display
            ImGui::Text("Type: %s", output->getTypeString());

            // Enable toggle — routed through OutputManager so the
            // physical output window actually gets created/destroyed.
            bool enabled = output->enabled;
            if (ImGui::Checkbox("Enabled", &enabled)) {
                if (auto* om = m_engine->getOutputManager()) {
                    om->setOutputEnabled(m_selectedOutput, enabled);
                } else {
                    output->enabled = enabled;
                }
            }

            // Resolution
            int resolution[2] = {output->width, output->height};
            if (ImGui::InputInt2("Resolution", resolution)) {
                output->width = std::max(1, resolution[0]);
                output->height = std::max(1, resolution[1]);
            }

            ImGui::Separator();

            // Input Region configuration
            ImGui::Text("Input Region");
            renderInputRegionConfig(*output);

            // Display assignment (for physical outputs)
            if (output->type == OutputType::Physical) {
                ImGui::Separator();
                ImGui::Text("Display Assignment");

                auto* om = m_engine->getOutputManager();
                if (om) {
                    const auto& displays = om->getAvailableDisplays();
                    int current = output->physicalDisplayIndex;
                    if (current < 0 || current >= static_cast<int>(displays.size())) {
                        current = -1;
                    }
                    const char* preview = (current >= 0 && current < static_cast<int>(displays.size()))
                        ? displays[current].displayName.c_str()
                        : "(no display)";
                    if (ImGui::BeginCombo("Display", preview)) {
                        for (int i = 0; i < static_cast<int>(displays.size()); ++i) {
                            const auto& d = displays[i];
                            char label[256];
                            std::snprintf(label, sizeof(label), "%d: %s (%dx%d%s)",
                                          i, d.displayName.c_str(),
                                          d.width, d.height,
                                          d.isPrimary ? ", primary" : "");
                            bool selected = (i == current);
                            if (ImGui::Selectable(label, selected)) {
                                om->assignDisplay(m_selectedOutput, i);
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        }
                        ImGui::EndCombo();
                    }
                    if (ImGui::Button("Refresh displays")) {
                        om->enumerateDisplays();
                    }
                } else {
                    ImGui::TextDisabled("(OutputManager not available)");
                }
            }

            // Source Screen routing — which Screen's compose target feeds
            // this output. Unset (entt::null) falls back to first visible
            // Screen in OutputManager::resolveSourceTexture().
            if (output->type == OutputType::Physical || output->type == OutputType::Preview) {
                ImGui::Separator();
                ImGui::Text("Source Screen");

                const char* sourcePreview = "(first visible screen)";
                std::string sourceLabel;
                if (output->sourceScreen != entt::null &&
                    registry.valid(output->sourceScreen) &&
                    registry.all_of<Screen>(output->sourceScreen)) {
                    const auto& s = registry.get<Screen>(output->sourceScreen);
                    sourceLabel = s.name + " (" +
                        std::to_string(s.width) + "x" + std::to_string(s.height) + ")";
                    sourcePreview = sourceLabel.c_str();
                }
                if (ImGui::BeginCombo("Screen##src", sourcePreview)) {
                    bool autoSelected = (output->sourceScreen == entt::null);
                    if (ImGui::Selectable("(first visible screen)", autoSelected)) {
                        output->sourceScreen = entt::null;
                    }
                    if (autoSelected) ImGui::SetItemDefaultFocus();

                    auto screenView = registry.view<Screen>();
                    for (auto [screenEntity, screen] : screenView.each()) {
                        char label[256];
                        std::snprintf(label, sizeof(label), "%s (%ux%u)",
                                      screen.name.c_str(), screen.width, screen.height);
                        bool sel = (output->sourceScreen == screenEntity);
                        ImGui::PushID(static_cast<int>(screenEntity));
                        if (ImGui::Selectable(label, sel)) {
                            output->sourceScreen = screenEntity;
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                        ImGui::PopID();
                    }
                    ImGui::EndCombo();
                }
            }

            // Color/gamma calibration. Pipeline order: brightness → OCIO
            // display+view ODT → gamma trim. Brightness is a pre-ODT scale,
            // gamma is a post-ODT calibration trim (kept for legacy projector
            // tuning workflows; not a colorimetric knob).
            ImGui::Separator();
            ImGui::Text("Calibration");
            ImGui::SliderFloat("Brightness##out", &output->brightness, 0.0f, 2.0f);

            // OCIO display + view dropdowns. Empty string = use the active
            // config's default at draw time. When OcioManager isn't bound
            // (config failed to load) we fall back to free-form text input
            // so the user can still set values that take effect on next
            // launch with a working config.
            auto* ocio = m_engine->getOcioManager();
            if (ocio && ocio->getConfig()) {
                const auto displays = ocio->listDisplays();
                const char* displayPreview = output->ocioDisplay.empty()
                    ? "(config default)"
                    : output->ocioDisplay.c_str();
                if (ImGui::BeginCombo("Display##out", displayPreview)) {
                    {
                        bool selected = output->ocioDisplay.empty();
                        if (ImGui::Selectable("(config default)", selected)) {
                            output->ocioDisplay.clear();
                            // Clearing the display invalidates the view choice
                            // (views are display-scoped). Drop the view too so
                            // the user doesn't end up with a stale pair.
                            output->ocioView.clear();
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    for (const auto& d : displays) {
                        bool selected = (d == output->ocioDisplay);
                        if (ImGui::Selectable(d.c_str(), selected)) {
                            if (d != output->ocioDisplay) {
                                output->ocioDisplay = d;
                                output->ocioView.clear();
                            }
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }

                std::string displayForViews = output->ocioDisplay.empty()
                    ? ocio->getDefaultDisplay()
                    : output->ocioDisplay;
                const auto views = ocio->listViews(displayForViews);
                const char* viewPreview = output->ocioView.empty()
                    ? "(config default)"
                    : output->ocioView.c_str();
                if (ImGui::BeginCombo("View##out", viewPreview)) {
                    {
                        bool selected = output->ocioView.empty();
                        if (ImGui::Selectable("(config default)", selected)) {
                            output->ocioView.clear();
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    for (const auto& v : views) {
                        bool selected = (v == output->ocioView);
                        if (ImGui::Selectable(v.c_str(), selected)) {
                            output->ocioView = v;
                        }
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                char dispBuf[128];
                strncpy(dispBuf, output->ocioDisplay.c_str(), sizeof(dispBuf) - 1);
                dispBuf[sizeof(dispBuf) - 1] = '\0';
                if (ImGui::InputText("Display##out", dispBuf, sizeof(dispBuf))) {
                    output->ocioDisplay = dispBuf;
                }
                char viewBuf[128];
                strncpy(viewBuf, output->ocioView.c_str(), sizeof(viewBuf) - 1);
                viewBuf[sizeof(viewBuf) - 1] = '\0';
                if (ImGui::InputText("View##out", viewBuf, sizeof(viewBuf))) {
                    output->ocioView = viewBuf;
                }
                ImGui::TextDisabled("OCIO config not loaded — dropdowns disabled.");
            }

            ImGui::SliderFloat("Gamma trim##out", &output->gamma, 0.5f, 2.5f);
        }
    } else {
        ImGui::TextDisabled("Select an output to edit");
    }
}

void MappingWindow::renderInputRegionConfig(OutputDisplay& output) {
    // Show input region as normalized values
    bool regionChanged = false;

    float regionValues[4] = {
        output.inputRegion.x,
        output.inputRegion.y,
        output.inputRegion.width,
        output.inputRegion.height
    };

    ImGui::SetNextItemWidth(120);
    if (ImGui::DragFloat2("Position##region", regionValues, 0.01f, 0.0f, 1.0f, "%.2f")) {
        regionChanged = true;
    }

    ImGui::SetNextItemWidth(120);
    if (ImGui::DragFloat2("Size##region", &regionValues[2], 0.01f, 0.01f, 1.0f, "%.2f")) {
        regionChanged = true;
    }

    if (regionChanged) {
        output.inputRegion.x = std::clamp(regionValues[0], 0.0f, 1.0f);
        output.inputRegion.y = std::clamp(regionValues[1], 0.0f, 1.0f);
        output.inputRegion.width = std::clamp(regionValues[2], 0.01f, 1.0f - output.inputRegion.x);
        output.inputRegion.height = std::clamp(regionValues[3], 0.01f, 1.0f - output.inputRegion.y);
    }

    // Preset buttons
    if (ImGui::Button("Full")) {
        output.inputRegion.reset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Left Half")) {
        output.inputRegion.x = 0.0f;
        output.inputRegion.y = 0.0f;
        output.inputRegion.width = 0.5f;
        output.inputRegion.height = 1.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Right Half")) {
        output.inputRegion.x = 0.5f;
        output.inputRegion.y = 0.0f;
        output.inputRegion.width = 0.5f;
        output.inputRegion.height = 1.0f;
    }

    // Show pixel values (read-only info)
    ImGui::TextDisabled("Pixels: %d,%d %dx%d",
                        output.inputRegion.pixelX, output.inputRegion.pixelY,
                        output.inputRegion.pixelWidth, output.inputRegion.pixelHeight);
}

} // namespace entity
