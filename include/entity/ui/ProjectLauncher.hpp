#pragma once

/**
 * ProjectLauncher — Recent / New / Open dialog rendered as a borderless
 * fullscreen ImGui surface. Per ADR-0009, Entity's editor only opens
 * with a valid project root; the launcher is the entry point that
 * provides one.
 *
 * Lifecycle: constructed by Engine and rendered every frame the engine
 * is in launcher mode (no project loaded). The launcher reads/writes
 * the RecentProjects file directly — Engine doesn't proxy that.
 *
 * After the user picks an action, render() returns a non-`None` Result.
 * Engine inspects the kind:
 *   - Open: load `path` into the editor, leave launcher mode.
 *   - Quit: requestExit; main loop returns.
 *
 * Both "New Project" and "Open Existing" paths flow through Result::Open
 * — the difference is whether the file already had content. New
 * Project's success path creates the v7 .entity on disk via
 * ProjectManager::createNew, then returns Open with the new file's
 * path so Engine just calls load() like any other open.
 *
 * The launcher renders a sub-modal for "New Project" (asks for parent
 * directory + project name). All modal state lives inside this class.
 *
 * GLFW window pointer is needed for the file-dialog parent HWND. We
 * use void* to avoid pulling in <GLFW/glfw3native.h> from the header.
 */

#include "entity/ui/FileDialog.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace entity {

class ProjectManager;
class RecentProjects;

class ProjectLauncher {
public:
    enum class Action {
        None,    // Still rendering; main loop continues
        Open,    // m_chosenPath is valid; engine should load it
        Quit,    // User wants out; engine should requestExit
    };

    struct Result {
        Action                action{Action::None};
        std::filesystem::path path;
    };

    /**
     * Wire the launcher to the project subsystems it needs. Safe to call
     * once. `glfwWindow` is used to refocus the editor window after a
     * floating native dialog dismisses itself (the dialog itself runs
     * unparented; see Issue #21).
     */
    void initialize(ProjectManager* projectManager,
                    RecentProjects* recentProjects,
                    void* glfwWindow);

    /**
     * Render one frame of the launcher. Must be called within an ImGui
     * frame (between NewFrame and Render).
     */
    Result render();

    /**
     * Reset internal modal state so the launcher can be shown again
     * (e.g. when the user closes a project from the editor and falls
     * back to the launcher). Safe to call before initialize().
     */
    void reset();

private:
    void renderNewProjectModal();
    void renderRecentList();

    bool tryCreateProject(std::string* errorOut);

    // Async dialog plumbing (Issue #21). Only one dialog can be in flight
    // at a time; UI gates duplicate launches via BeginDisabled while
    // m_pendingDialog != nullptr.
    void pumpPendingDialog();
    void startDialog(std::unique_ptr<ui::FileDialogTask> task,
                     std::function<void(std::filesystem::path)> onComplete);

    ProjectManager* m_projectManager{nullptr};
    RecentProjects* m_recentProjects{nullptr};
    void*           m_glfwWindow{nullptr};

    // Pending result the next render() will return. Set by button
    // handlers; consumed at the bottom of render().
    Result m_pending;

    // "New Project" modal state.
    bool        m_newProjectOpen{false};
    std::string m_newProjectName;
    std::string m_newProjectParentDir;
    std::string m_newProjectError;

    // Hovered/selected entry for the Recent list (used to drive a "Forget"
    // affordance later — currently unused but reserved so right-click
    // context menus in v2 don't churn the API).
    int m_hoveredRecent{-1};

    std::unique_ptr<ui::FileDialogTask>            m_pendingDialog;
    std::function<void(std::filesystem::path)>     m_pendingDialogCallback;
};

}  // namespace entity
