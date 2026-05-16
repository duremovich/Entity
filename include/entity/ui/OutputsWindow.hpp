#pragma once

#include "EditorWindow.hpp"
#include "entity/components/OutputSurface.hpp"
#include "entity/components/OutputDisplay.hpp"
#include <imgui.h>
#include <entt/entt.hpp>
#include <vector>

namespace entity {

// Forward declarations
class Engine;
class OutputManager;

/**
 * OutputsWindow - Plane B feed-output editor (ADR-0021 M4).
 *
 * Renamed from `MappingWindow` to match the two-tier mapping vocabulary —
 * this window is Plane B (screens/projectors → physical outputs + warp),
 * not Plane A (content → screens). Plane A authoring lives in
 * `ContentRoutingWindow`.
 *
 * Provides interactive editing of:
 * - OutputDisplay enable / physical-display assignment / source routing
 * - Per-output InputRegion crop + OCIO / brightness / gamma calibration
 * - OutputSurface corner-pin warp quads (one or many per output)
 * - Surface soft-edge feather + source-region sub-UVs
 */
class OutputsWindow : public EditorWindow {
public:
    explicit OutputsWindow(Engine* engine);
    ~OutputsWindow() override = default;

    void render() override;
    const char* getName() const override { return "Outputs"; }

    ImGuiWindowFlags getWindowFlags() const override {
        return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

    void applyPreBeginStyles() const override {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    }

    void popPreBeginStyles() const override {
        ImGui::PopStyleVar();
    }

private:
    /**
     * Render the canvas area with video preview and surfaces.
     */
    void renderCanvas();

    /**
     * Render a single mapping surface.
     */
    void renderSurface(entt::entity entity, OutputSurface& surface, ImDrawList* drawList,
                       const ImVec2& canvasPos, const ImVec2& canvasSize);

    /**
     * Render corner handles for a surface.
     */
    void renderCornerHandles(OutputSurface& surface, ImDrawList* drawList,
                             const ImVec2& canvasPos, const ImVec2& canvasSize);

    /**
     * Render the toolbar with surface controls.
     */
    void renderToolbar();

    /**
     * Render surface properties panel.
     */
    void renderPropertiesPanel();

    /**
     * Render the outputs sidebar panel.
     */
    void renderOutputsPanel();

    /**
     * Render input region configuration for an output.
     */
    void renderInputRegionConfig(OutputDisplay& output);

    /**
     * Handle mouse interaction (dragging corners, selecting surfaces).
     */
    void handleInteraction(const ImVec2& canvasPos, const ImVec2& canvasSize);

    /**
     * Convert normalized coordinates (-1 to 1) to canvas pixel position.
     */
    ImVec2 normalizedToCanvas(const glm::vec2& pos, const ImVec2& canvasPos, const ImVec2& canvasSize) const;

    /**
     * Convert canvas pixel position to normalized coordinates (-1 to 1).
     */
    glm::vec2 canvasToNormalized(const ImVec2& pos, const ImVec2& canvasPos, const ImVec2& canvasSize) const;

    /**
     * Create a new mapping surface entity.
     */
    entt::entity createSurface(const std::string& name);

    /**
     * Delete the selected surface.
     */
    void deleteSelectedSurface();

private:
    Engine* m_engine{nullptr};

    // Selection state
    entt::entity m_selectedSurface{entt::null};
    entt::entity m_selectedOutput{entt::null};
    int m_draggingCorner{-1};  // -1 = not dragging, 0-3 = corner index
    bool m_isDraggingSurface{false};
    glm::vec2 m_dragOffset{0.0f, 0.0f};

    // View settings
    bool m_showGrid{true};
    bool m_showHandles{true};
    bool m_showSoftEdge{false};
    bool m_showOutputsPanel{true};

    // Corner handle visual size
    static constexpr float CORNER_HANDLE_SIZE = 10.0f;
    static constexpr float CORNER_HIT_SIZE = 15.0f;  // Larger hit area

    // Colors
    static constexpr ImU32 COLOR_SURFACE_OUTLINE = IM_COL32(100, 200, 100, 255);
    static constexpr ImU32 COLOR_SURFACE_SELECTED = IM_COL32(255, 200, 50, 255);
    static constexpr ImU32 COLOR_SURFACE_FILL = IM_COL32(100, 200, 100, 30);
    static constexpr ImU32 COLOR_CORNER_HANDLE = IM_COL32(255, 255, 255, 255);
    static constexpr ImU32 COLOR_CORNER_ACTIVE = IM_COL32(255, 200, 50, 255);
    static constexpr ImU32 COLOR_GRID = IM_COL32(80, 80, 80, 100);
};

} // namespace entity
