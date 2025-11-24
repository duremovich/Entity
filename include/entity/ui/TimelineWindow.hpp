#pragma once

#include "EditorWindow.hpp"
#include "entity/timeline/TimelineWidget.hpp"
#include <memory>

namespace entity {

/**
 * TimelineWindow - Dockable window wrapper for the Timeline widget.
 *
 * Wraps the existing TimelineWidget into the EditorWindow interface,
 * making it dockable and manageable by the WindowManager.
 */
class TimelineWindow : public EditorWindow {
public:
    explicit TimelineWindow(Timeline* timeline);
    ~TimelineWindow() override = default;

    void render() override;
    const char* getName() const override { return "Timeline"; }

    ImGuiWindowFlags getWindowFlags() const override {
        // NoScrollbar - timeline handles its own scrolling
        // NoScrollWithMouse - prevent accidental scroll when interacting with timeline
        return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

    void applyPreBeginStyles() const override {
        // Remove window padding so timeline content is flush with window edges
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    }

    void popPreBeginStyles() const override {
        ImGui::PopStyleVar();
    }

private:
    std::unique_ptr<TimelineWidget> m_widget;
};

} // namespace entity
