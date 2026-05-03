#pragma once

#include "EditorWindow.hpp"
#include "ProjectorCalibrationWindow.hpp"
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
     * Get/set the current view mode. The setter routes through
     * requestViewMode() so a mode change auto-frames the new view.
     */
    StageViewMode getViewMode() const { return m_viewMode; }
    void setViewMode(StageViewMode mode) { requestViewMode(mode); }

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

    /**
     * Request a view-mode transition. No-op if already in `mode`.
     * Otherwise switches and flags an auto-frame for the next render.
     */
    void requestViewMode(StageViewMode mode);

    /**
     * If an auto-frame was requested (or this is first-render with
     * content), reset 2D pan/zoom or call frameAllScreens() in 3D.
     * Called from render() before dispatching to the view.
     */
    void applyAutoFrame(ImVec2 viewSize);

    /**
     * Collect every visible Screen entity's transform and call
     * Stage3DRenderer::frameScreens(). Falls back to camera Reset on
     * empty stage. Caller must have set the renderer's camera aspect
     * ratio for `viewSize` first.
     */
    void frameAllScreens(ImVec2 viewSize);

    /**
     * Reset 2D pan/zoom to fit-to-window.
     */
    void reset2DView();

private:
    Engine* m_engine{nullptr};  // Non-owning pointer to Engine
    StageViewMode m_viewMode{StageViewMode::View3D};  // Default to 3D view
    std::unique_ptr<Stage3DRenderer> m_3dRenderer;
    std::unique_ptr<ProjectorCalibrationWindow> m_calibWindow;

    // 2D view pan/zoom state. zoom is a multiplier on the
    // aspect-fit-to-window size; pan is a pixel offset from centered.
    float m_2dZoom{1.0f};
    ImVec2 m_2dPan{0.0f, 0.0f};
    bool m_2dPanning{false};

    // Auto-frame triggers. Set when view mode changes; cleared after
    // applyAutoFrame() runs in the next render(). m_didInitialFrame
    // ensures we frame on first render once the engine has any Screens.
    bool m_pendingAutoFrame{false};
    bool m_didInitialFrame{false};

    static constexpr float MIN_2D_ZOOM = 0.1f;
    static constexpr float MAX_2D_ZOOM = 16.0f;
};

} // namespace entity
