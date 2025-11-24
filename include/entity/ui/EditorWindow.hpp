#pragma once

#include <imgui.h>
#include <string>

namespace entity {

/**
 * EditorWindow - Abstract base class for all dockable editor windows.
 *
 * All UI windows (Timeline, Stage, Properties, etc.) inherit from this class.
 * Provides common interface for window management, visibility, and rendering.
 */
class EditorWindow {
public:
    virtual ~EditorWindow() = default;

    /**
     * Render the window contents.
     * Called every frame by WindowManager.
     */
    virtual void render() = 0;

    /**
     * Get the unique name of this window.
     * Used for ImGui window identification and docking.
     */
    virtual const char* getName() const = 0;

    /**
     * Check if window is currently visible.
     */
    bool isVisible() const { return m_visible; }

    /**
     * Set window visibility.
     * When false, window is not rendered.
     */
    void setVisible(bool visible) { m_visible = visible; }

    /**
     * Get ImGui window flags for this window.
     * Override to customize window behavior (e.g., no scrollbar, no resize).
     */
    virtual ImGuiWindowFlags getWindowFlags() const {
        return ImGuiWindowFlags_None;
    }

    /**
     * Apply ImGui styles before ImGui::Begin() is called.
     * Override to set window padding, rounding, etc.
     * Must be paired with popPreBeginStyles().
     */
    virtual void applyPreBeginStyles() const {}

    /**
     * Pop ImGui styles after ImGui::End() is called.
     * Must match the number of Push calls in applyPreBeginStyles().
     */
    virtual void popPreBeginStyles() const {}

protected:
    bool m_visible{true};
};

} // namespace entity
