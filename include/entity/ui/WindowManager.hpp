#pragma once

#include "EditorWindow.hpp"
#include "SettingsWindow.hpp"
#include "entity/core/Settings.hpp"
#include "entity/ui/FileDialog.hpp"
#include <imgui.h>
#include <filesystem>
#include <memory>
#include <vector>
#include <functional>
#include <string>
#include <unordered_set>

namespace entity {

class OcioManager;

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
     * Lock/unlock the window layout.
     * When locked, docked windows cannot be moved or undocked via dragging.
     * Use undockWindow() to programmatically undock specific windows.
     */
    void setLayoutLocked(bool locked) { m_layoutLocked = locked; }
    bool isLayoutLocked() const { return m_layoutLocked; }

    /**
     * Request to undock a specific window (make it floating).
     * The window will be undocked on the next frame.
     */
    void undockWindow(const char* name);

    /**
     * Request to re-dock a specific window back to the main dockspace.
     * The window will be docked on the next frame.
     */
    void dockWindow(const char* name);

    /**
     * Check if a window is currently floating (undocked).
     */
    bool isWindowFloating(const char* name) const;

    /**
     * Set callback for when a video file is selected.
     * Callback receives the file path.
     */
    using VideoFileCallback = std::function<void(const std::string&)>;
    void setVideoFileCallback(VideoFileCallback callback) { m_videoFileCallback = callback; }

    /**
     * Set callback for save project action (Ctrl+S / "File > Save Project").
     * Engine decides whether to save to the current path or prompt Save As.
     */
    using SaveProjectCallback = std::function<void()>;
    void setSaveProjectCallback(SaveProjectCallback callback) { m_saveProjectCallback = callback; }

    /**
     * Set callback for save-project-as action (Ctrl+Shift+S / "File > Save Project As...").
     * Engine always prompts for a path.
     */
    using SaveProjectAsCallback = std::function<void()>;
    void setSaveProjectAsCallback(SaveProjectAsCallback callback) { m_saveProjectAsCallback = callback; }

    /**
     * Set callback for open project action.
     * Callback receives the selected file path.
     */
    using OpenProjectCallback = std::function<void(const std::string&)>;
    void setOpenProjectCallback(OpenProjectCallback callback) { m_openProjectCallback = callback; }

    /**
     * ADR-0009 — File > Close Project. Engine tears down the editor's
     * project-scoped state and re-shows the Project Launcher. The bool
     * callback gates the menu item (disabled when no project is loaded).
     */
    using CloseProjectCallback   = std::function<void()>;
    using CanCloseProjectCallback = std::function<bool()>;
    void setCloseProjectCallback(CloseProjectCallback cb)       { m_closeProjectCallback   = std::move(cb); }
    void setCanCloseProjectCallback(CanCloseProjectCallback cb) { m_canCloseProjectCallback = std::move(cb); }

    /**
     * Set callback for exit action.
     */
    using ExitCallback = std::function<void()>;
    void setExitCallback(ExitCallback callback) { m_exitCallback = callback; }

    /**
     * Set callback for run script action.
     * Callback receives the selected script file path.
     */
    using RunScriptCallback = std::function<void(const std::string&)>;
    void setRunScriptCallback(RunScriptCallback callback) { m_runScriptCallback = callback; }

    /**
     * ADR-0009 — "Collect Linked Media into Project Folder" menu action.
     * The bool callback returns whether the menu item should be enabled
     * (Engine: count of Linked entries > 0). The void callback fires
     * when the user clicks; Engine runs collectLinkedIntoProject().
     */
    using CollectMediaCallback   = std::function<void()>;
    using CanCollectMediaCallback = std::function<bool()>;
    void setCollectMediaCallback(CollectMediaCallback cb)       { m_collectMediaCallback   = std::move(cb); }
    void setCanCollectMediaCallback(CanCollectMediaCallback cb) { m_canCollectMediaCallback = std::move(cb); }

    /**
     * ADR-0009 — "Save Project As Bundle..." menu action.
     * Returns true on success (modal closes), false to keep the modal
     * open with a displayed error. Engine runs `saveAsBundle` and
     * `recentProjects->touch` on the new file.
     */
    using SaveBundleCallback = std::function<bool(const std::string& parentDir,
                                                   const std::string& projectName,
                                                   std::string* errorOut)>;
    void setSaveBundleCallback(SaveBundleCallback cb) { m_saveBundleCallback = std::move(cb); }

    /**
     * Engine calls this to seed the bundle modal's defaults: the parent
     * dir is pre-populated from the suggested path; project name from
     * the current project's stem.
     */
    using CurrentProjectInfoCallback =
        std::function<std::pair<std::string, std::string>()>;  // (parentDir, name)
    void setCurrentProjectInfoCallback(CurrentProjectInfoCallback cb) {
        m_currentProjectInfoCallback = std::move(cb);
    }

    /**
     * ADR-0009 — orphan-showfile recovery hooks.
     *
     * `RebuildStructureCallback` — File > "Rebuild Project Structure".
     * Engine: idempotent mkdir of canonical subdirs.
     *
     * `FindMissingMediaCallback` — File > "Find Missing Media...".
     * Engine: recursive search of the user-picked folder, copies
     * matches into canonical content paths, reloads on success.
     */
    using RebuildStructureCallback = std::function<int()>;
    using FindMissingMediaCallback = std::function<int(const std::string& searchDir)>;
    void setRebuildStructureCallback(RebuildStructureCallback cb) {
        m_rebuildStructureCallback = std::move(cb);
    }
    void setFindMissingMediaCallback(FindMissingMediaCallback cb) {
        m_findMissingMediaCallback = std::move(cb);
    }

    /**
     * Edit menu — ripple insert / delete time on the active range selection.
     * Engine wires these to read TimelineWidget's range and dispatch the
     * Ripple{Insert,Delete}TimeCommand through CommandDispatcher (so they
     * land on the undo stack like any other UI-driven edit).
     *
     * The bool returned by hasRangeSelectionCallback gates the menu's enable
     * state — disabled when there's no active selection.
     */
    using HasRangeSelectionCallback = std::function<bool()>;
    using RippleInsertCallback = std::function<void()>;
    using RippleDeleteCallback = std::function<void()>;
    using UndoCallback = std::function<void()>;
    using RedoCallback = std::function<void()>;
    using CanUndoCallback = std::function<bool()>;
    using CanRedoCallback = std::function<bool()>;
    void setHasRangeSelectionCallback(HasRangeSelectionCallback cb) { m_hasRangeSelectionCallback = std::move(cb); }
    void setRippleInsertCallback(RippleInsertCallback cb) { m_rippleInsertCallback = std::move(cb); }
    void setRippleDeleteCallback(RippleDeleteCallback cb) { m_rippleDeleteCallback = std::move(cb); }
    void setUndoCallback(UndoCallback cb) { m_undoCallback = std::move(cb); }
    void setRedoCallback(RedoCallback cb) { m_redoCallback = std::move(cb); }
    void setCanUndoCallback(CanUndoCallback cb) { m_canUndoCallback = std::move(cb); }
    void setCanRedoCallback(CanRedoCallback cb) { m_canRedoCallback = std::move(cb); }

    /**
     * Settings/Preferences integration. Engine owns the live Settings; the
     * Edit > Preferences... menu item opens a modal seeded with `currentSettings()`,
     * and `onSettingsApplied(newValues)` fires when the user clicks OK.
     */
    using CurrentSettingsCallback = std::function<Settings()>;
    using SettingsAppliedCallback = std::function<void(const Settings&)>;
    void setCurrentSettingsCallback(CurrentSettingsCallback cb) { m_currentSettingsCallback = std::move(cb); }
    void setSettingsAppliedCallback(SettingsAppliedCallback cb);

    /**
     * Hand the OcioManager to the Preferences modal so its Color section
     * can populate color-space / display / view dropdowns from the active
     * config. Pointer is non-owning; caller (Engine) keeps OcioManager
     * alive for the SettingsWindow's lifetime.
     */
    void setOcioManager(OcioManager* mgr) { m_settingsWindow.setOcioManager(mgr); }

    /**
     * Native file dialogs (Issue #21). The dialog runs on a worker thread
     * and the result is delivered to `onPicked` from the main thread on a
     * subsequent frame. `chosenPath` is empty if the user cancelled or the
     * dialog errored.
     *
     * Only one dialog can be in flight at a time; calls made while another
     * dialog is pending are silently ignored. Callers should gate UI
     * (e.g. `ImGui::BeginDisabled(isFileDialogPending())`) to avoid
     * double-fires.
     */
    using PathCallback = std::function<void(const std::string& chosenPath)>;
    void openVideoFileDialog(PathCallback onPicked);
    void openProjectFileDialog(PathCallback onPicked);
    void saveProjectFileDialog(const std::string& suggestedPath,
                                PathCallback onPicked);
    void openScriptFileDialog(PathCallback onPicked);
    void openOBJFileDialog(PathCallback onPicked);
    void openOcioConfigFileDialog(PathCallback onPicked);

    /** True while a native file dialog is open. UI should disable buttons that would launch another. */
    bool isFileDialogPending() const { return m_pendingDialog != nullptr; }

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

private:
    std::vector<std::unique_ptr<EditorWindow>> m_windows;
    ImGuiID m_dockspaceId{0};
    bool m_firstFrame{true};
    bool m_layoutResetRequested{false};
    bool m_layoutLocked{true};  // Default: layout locked to prevent accidental undocking

    // Per-window floating state (windows explicitly undocked by user)
    std::unordered_set<std::string> m_floatingWindows;      // Currently floating windows
    std::unordered_set<std::string> m_pendingUndock;        // Windows to undock next frame
    std::unordered_set<std::string> m_pendingDock;          // Windows to dock next frame

    VideoFileCallback m_videoFileCallback;
    SaveProjectCallback m_saveProjectCallback;
    SaveProjectAsCallback m_saveProjectAsCallback;
    OpenProjectCallback m_openProjectCallback;
    CloseProjectCallback    m_closeProjectCallback;
    CanCloseProjectCallback m_canCloseProjectCallback;
    ExitCallback m_exitCallback;
    RunScriptCallback m_runScriptCallback;
    CollectMediaCallback   m_collectMediaCallback;
    CanCollectMediaCallback m_canCollectMediaCallback;
    SaveBundleCallback         m_saveBundleCallback;
    CurrentProjectInfoCallback m_currentProjectInfoCallback;
    RebuildStructureCallback   m_rebuildStructureCallback;
    FindMissingMediaCallback   m_findMissingMediaCallback;

    // Save-As-Bundle modal (ADR-0009). Open state + form fields persist
    // across frames while the popup is up; reset when the user clicks
    // Save or Cancel.
    bool        m_saveBundleModalOpen{false};
    bool        m_saveBundleModalSeed{false};
    std::string m_saveBundleParentDir;
    std::string m_saveBundleName;
    std::string m_saveBundleError;
    void renderSaveBundleModal();

    HasRangeSelectionCallback m_hasRangeSelectionCallback;
    RippleInsertCallback m_rippleInsertCallback;
    RippleDeleteCallback m_rippleDeleteCallback;
    UndoCallback m_undoCallback;
    RedoCallback m_redoCallback;
    CanUndoCallback m_canUndoCallback;
    CanRedoCallback m_canRedoCallback;

    // Preferences modal — owned by WindowManager so the Edit > Preferences...
    // menu item can pop it open without per-frame plumbing through Engine.
    SettingsWindow            m_settingsWindow;
    CurrentSettingsCallback   m_currentSettingsCallback;
    SettingsAppliedCallback   m_settingsAppliedCallback;

    // Async file-dialog plumbing (Issue #21). Single slot — one dialog at
    // a time. pumpPendingDialog() drains a completed task at the top of
    // render(); startDialog() registers a new one.
    std::unique_ptr<ui::FileDialogTask>            m_pendingDialog;
    std::function<void(std::filesystem::path)>     m_pendingDialogCallback;

    void pumpPendingDialog();
    void startDialog(std::unique_ptr<ui::FileDialogTask> task,
                     std::function<void(std::filesystem::path)> onComplete);
};

} // namespace entity
