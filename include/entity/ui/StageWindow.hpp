#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>

namespace entity {

// Forward declarations
class Engine;

/**
 * StageWindow - Main video output/preview window.
 *
 * Displays the composited video output from the renderer.
 * Shows decoded video frames from the Engine's current frame buffer.
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

private:
    Engine* m_engine{nullptr};  // Non-owning pointer to Engine
};

} // namespace entity
