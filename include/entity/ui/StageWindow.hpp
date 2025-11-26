#pragma once

#include "EditorWindow.hpp"
#include "../render/Stage3DRenderer.hpp"
#include <imgui.h>
#include <memory>

namespace entity {

// Forward declarations
class Engine;

/**
 * View mode for the stage window
 */
enum class StageViewMode {
    View2D,     // Traditional 2D composited output view
    View3D      // 3D stage visualization with floor and screen
};

/**
 * StageWindow - Main video output/preview window.
 *
 * Displays the composited video output from the renderer.
 * Supports both 2D (flat composited view) and 3D (stage visualization) modes.
 */
class StageWindow : public EditorWindow {
public:
    explicit StageWindow(Engine* engine);
    ~StageWindow() override = default;

    void render() override;
    const char* getName() const override { return "Stage"; }

    ImGuiWindowFlags getWindowFlags() const override {
        // NoScrollbar - stage should fill entire window
        // NoScrollWithMouse - prevent accidental scroll
        return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

    void applyPreBeginStyles() const override {
        // Remove window padding so video fills the entire window
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    }

    void popPreBeginStyles() const override {
        ImGui::PopStyleVar();
    }

    /**
     * Get/set the current view mode.
     */
    StageViewMode getViewMode() const { return m_viewMode; }
    void setViewMode(StageViewMode mode) { m_viewMode = mode; }

    /**
     * Get the 3D renderer for camera manipulation.
     */
    Stage3DRenderer* get3DRenderer() { return m_3dRenderer.get(); }

private:
    /**
     * Render the 2D composited view.
     */
    void render2DView();

    /**
     * Render the 3D stage visualization.
     */
    void render3DView();

    /**
     * Render the view mode toolbar.
     */
    void renderToolbar();

private:
    Engine* m_engine{nullptr};  // Non-owning pointer to Engine
    StageViewMode m_viewMode{StageViewMode::View3D};  // Default to 3D view
    std::unique_ptr<Stage3DRenderer> m_3dRenderer;
};

} // namespace entity
