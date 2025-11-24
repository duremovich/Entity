#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>
#include <memory>
#include <vector>
#include <functional>
#include <string>

namespace entity {

/**
 * WindowManager - Central manager for all dockable editor windows.
 *
 * Responsibilities:
 * - Creates and manages the main DockSpace
 * - Registers and renders all editor windows
 * - Creates default layout on first run
 * - Provides menu bar for window visibility toggles
 */
class WindowManager {
public:
    WindowManager();
    ~WindowManager();

    /**
     * Initialize the window manager.
     */
    void initialize();

    /**
     * Shutdown and clean up windows.
     */
    void shutdown();

    /**
     * Render the DockSpace and all registered windows.
     * Called every frame by Engine.
     */
    void render();

    /**
     * Register a new editor window.
     * WindowManager takes ownership of the window.
     */
    void registerWindow(std::unique_ptr<EditorWindow> window);

    /**
     * Set visibility of a window by name.
     */
    void setWindowVisible(const char* name, bool visible);

    /**
     * Reset layout to default configuration.
     */
    void resetLayout();

    /**
     * Set callback for when a video file is selected.
     * Callback receives the file path.
     */
    using VideoFileCallback = std::function<void(const std::string&)>;
    void setVideoFileCallback(VideoFileCallback callback) { m_videoFileCallback = callback; }

private:
    /**
     * Render the menu bar with window visibility toggles.
     */
    void renderMenuBar();

    /**
     * Create the default docked window layout.
     * Called on first frame or after layout reset.
     */
    void createDefaultLayout(ImGuiID dockspaceId);

    /**
     * Open Windows native file dialog for video selection.
     * Returns the selected file path, or empty string if cancelled.
     */
    std::string openVideoFileDialog();

private:
    std::vector<std::unique_ptr<EditorWindow>> m_windows;
    ImGuiID m_dockspaceId{0};
    bool m_firstFrame{true};
    bool m_layoutResetRequested{false};
    VideoFileCallback m_videoFileCallback;
};

} // namespace entity
