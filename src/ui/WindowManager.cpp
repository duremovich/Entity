#include "entity/ui/WindowManager.hpp"
#include "entity/ui/FileDialog.hpp"
#include <imgui_internal.h>  // For DockBuilder API
#include <cstdio>
#include <filesystem>
#include <iostream>

namespace {

// std::filesystem::path::string() uses the system ANSI codepage on Windows
// and throws std::filesystem::filesystem_error when the native wide path
// contains characters outside that codepage — fullwidth punctuation, curly
// quotes, emoji, CJK, etc. Since IFileDialog lets users pick any file, we
// must not crash on such paths. Convert to UTF-8 instead; modern FFmpeg on
// Windows decodes UTF-8 path arguments correctly.
std::string pathToUtf8(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

}  // namespace

namespace entity {

WindowManager::WindowManager() {
    // Constructor
}

WindowManager::~WindowManager() {
    shutdown();
}

void WindowManager::initialize() {
    std::cout << "Initializing WindowManager..." << std::endl;
    m_firstFrame = true;
    m_layoutResetRequested = false;
}

void WindowManager::shutdown() {
    std::cout << "Shutting down WindowManager..." << std::endl;
    m_windows.clear();
}

void WindowManager::render() {
    // Create fullscreen dockspace over the main viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    // Dockspace window flags
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
    windowFlags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    windowFlags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    // Important: Set padding to 0 so dockspace fills entire window
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    // Begin the dockspace window
    ImGui::Begin("DockSpace Window", nullptr, windowFlags);
    ImGui::PopStyleVar();

    // Render menu bar
    renderMenuBar();

    // Create the dockspace
    ImGuiID dockspaceID = ImGui::GetID("MainDockSpace");
    m_dockspaceId = dockspaceID;

    // Create default layout on first frame or after reset
    if (m_firstFrame || m_layoutResetRequested) {
        createDefaultLayout(dockspaceID);
        m_firstFrame = false;
        m_layoutResetRequested = false;
    }

    // Create the dockspace node
    ImGui::DockSpace(dockspaceID, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    ImGui::End();

    // Render all registered windows
    for (auto& window : m_windows) {
        if (window->isVisible()) {
            const char* windowName = window->getName();
            std::string windowNameStr(windowName);

            // Process pending undock requests
            if (m_pendingUndock.count(windowNameStr) > 0) {
                ImGui::SetNextWindowDockID(0, ImGuiCond_Always);  // 0 = floating

                // Shrink and offset slightly to visually indicate undocking
                ImGuiWindow* existingWin = ImGui::FindWindowByName(windowName);
                if (existingWin) {
                    constexpr float shrinkAmount = 10.0f;  // Pixels to shrink on each side
                    ImVec2 newSize(
                        existingWin->Size.x - shrinkAmount * 2,
                        existingWin->Size.y - shrinkAmount * 2
                    );
                    ImVec2 newPos(
                        existingWin->Pos.x + shrinkAmount,
                        existingWin->Pos.y + shrinkAmount
                    );
                    ImGui::SetNextWindowSize(newSize, ImGuiCond_Always);
                    ImGui::SetNextWindowPos(newPos, ImGuiCond_Always);
                }

                m_floatingWindows.insert(windowNameStr);
                m_pendingUndock.erase(windowNameStr);
                std::cout << "Undocking window: " << windowName << std::endl;
            }

            // Process pending dock requests
            if (m_pendingDock.count(windowNameStr) > 0) {
                ImGui::SetNextWindowDockID(dockspaceID, ImGuiCond_Always);
                m_floatingWindows.erase(windowNameStr);
                m_pendingDock.erase(windowNameStr);
                std::cout << "Docking window: " << windowName << std::endl;
            }

            // Apply pre-begin styles (e.g., window padding)
            window->applyPreBeginStyles();

            // Begin window with custom flags
            ImGuiWindowFlags flags = window->getWindowFlags();

            // When layout is locked AND window is docked, prevent movement
            // Floating windows should always be movable
            bool isFloating = m_floatingWindows.count(windowNameStr) > 0;
            if (m_layoutLocked && !isFloating) {
                flags |= ImGuiWindowFlags_NoMove;
            }

            if (ImGui::Begin(windowName, nullptr, flags)) {
                // Right-click context menu for undock/dock
                // Use internal API to detect right-click on title bar or tab
                ImGuiWindow* imguiWin = ImGui::GetCurrentWindow();

                // Check actual dock state from ImGui (not our tracking variable)
                // This ensures menu always reflects reality even after manual drag operations
                bool actuallyFloating = (imguiWin && imguiWin->DockId == 0);

                // Sync our tracking with reality
                if (actuallyFloating && !isFloating) {
                    m_floatingWindows.insert(windowNameStr);
                } else if (!actuallyFloating && isFloating) {
                    m_floatingWindows.erase(windowNameStr);
                }

                if (imguiWin && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                    ImVec2 mousePos = ImGui::GetMousePos();
                    bool showMenu = false;

                    // For floating windows: check title bar rect
                    if (actuallyFloating) {
                        ImRect titleBarRect = imguiWin->TitleBarRect();
                        showMenu = titleBarRect.Contains(mousePos);
                    } else {
                        // For docked windows: check if we clicked on this window's tab
                        ImGuiDockNode* dockNode = imguiWin->DockNode;
                        if (dockNode && dockNode->TabBar) {
                            ImGuiTabBar* tabBar = dockNode->TabBar;
                            // Check if mouse is in tab bar area and this is the active/hovered tab
                            ImRect tabBarRect = ImRect(dockNode->Pos, ImVec2(dockNode->Pos.x + dockNode->Size.x,
                                                                              dockNode->Pos.y + tabBar->BarRect.GetHeight()));
                            if (tabBarRect.Contains(mousePos)) {
                                // Check if this specific window's tab is being hovered
                                for (int i = 0; i < tabBar->Tabs.Size; i++) {
                                    ImGuiTabItem& tab = tabBar->Tabs[i];
                                    if (tab.Window == imguiWin) {
                                        // Calculate tab rect
                                        ImRect tabRect(tab.Offset + tabBar->BarRect.Min.x, tabBar->BarRect.Min.y,
                                                       tab.Offset + tab.Width + tabBar->BarRect.Min.x, tabBar->BarRect.Max.y);
                                        if (tabRect.Contains(mousePos)) {
                                            showMenu = true;
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                    }

                    if (showMenu) {
                        ImGui::OpenPopup("WindowContextMenu");
                    }
                }

                if (ImGui::BeginPopup("WindowContextMenu")) {
                    ImGui::TextDisabled("%s", windowName);
                    ImGui::Separator();

                    // Use actual dock state for menu options
                    if (actuallyFloating) {
                        if (ImGui::MenuItem("Dock Window")) {
                            dockWindow(windowName);
                        }
                    } else {
                        if (ImGui::MenuItem("Undock Window")) {
                            undockWindow(windowName);
                        }
                    }
                    ImGui::EndPopup();
                }

                window->render();
            }
            ImGui::End();

            // Pop pre-begin styles
            window->popPreBeginStyles();
        }
    }

    // Preferences modal — rendered last so it draws over the dockspace + windows.
    // No-op when not open; cheap to call every frame.
    m_settingsWindow.render();

    // ADR-0009 — Save-As-Bundle modal. No-op when m_saveBundleModalOpen is false.
    if (m_saveBundleModalOpen) {
        renderSaveBundleModal();
    }
}

void WindowManager::renderSaveBundleModal() {
    namespace fs = std::filesystem;

    // Seed defaults the first frame after opening: project name from current
    // project's stem; parent dir from current project's parent (or the OS
    // user-documents fallback).
    if (m_saveBundleModalSeed) {
        std::string seedName, seedParent;
        if (m_currentProjectInfoCallback) {
            auto info = m_currentProjectInfoCallback();
            seedParent = info.first;
            seedName   = info.second;
        }
        if (seedParent.empty()) {
#ifdef _WIN32
            if (const char* userprofile = std::getenv("USERPROFILE")) {
                seedParent = (fs::path(userprofile) / "Documents" / "Entity").string();
            }
#else
            if (const char* home = std::getenv("HOME")) {
                seedParent = (fs::path(home) / "Entity").string();
            }
#endif
        }
        m_saveBundleParentDir = seedParent;
        m_saveBundleName      = seedName;
        m_saveBundleModalSeed = false;
    }

    ImGui::OpenPopup("Save Project As Bundle");

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                           viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0));

    if (ImGui::BeginPopupModal("Save Project As Bundle", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "Creates a new project folder containing a copy of the .entity file, "
            "content/, presets/, objects/, exports/, and snapshots/. The current "
            "project stays untouched until you save it again.");
        ImGui::Spacing();

        ImGui::Text("Project Name");
        char nameBuf[256];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", m_saveBundleName.c_str());
        if (ImGui::InputText("##bundle-name", nameBuf, sizeof(nameBuf))) {
            m_saveBundleName = nameBuf;
        }
        ImGui::TextDisabled("Letters, numbers, dashes — no path separators.");
        ImGui::Spacing();

        ImGui::Text("Parent Directory");
        char parentBuf[1024];
        std::snprintf(parentBuf, sizeof(parentBuf), "%s", m_saveBundleParentDir.c_str());
        if (ImGui::InputText("##bundle-parent", parentBuf, sizeof(parentBuf))) {
            m_saveBundleParentDir = parentBuf;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...##bundle")) {
            fs::path initial = m_saveBundleParentDir.empty()
                                   ? fs::path{}
                                   : fs::path(m_saveBundleParentDir);
            fs::path picked = ui::pickFolderDialog(
                m_ownerWindow, L"Choose parent directory for the bundle", initial);
            if (!picked.empty()) {
                m_saveBundleParentDir = pathToUtf8(picked);
            }
        }

        if (!m_saveBundleName.empty() && !m_saveBundleParentDir.empty()) {
            fs::path preview = fs::path(m_saveBundleParentDir) / m_saveBundleName;
            ImGui::TextDisabled("Will create: %s", preview.string().c_str());
        }

        if (!m_saveBundleError.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                               m_saveBundleError.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool canSave = !m_saveBundleName.empty()
                          && !m_saveBundleParentDir.empty()
                          && m_saveBundleCallback;
        ImGui::BeginDisabled(!canSave);
        if (ImGui::Button("Save Bundle", ImVec2(140, 0))) {
            std::string err;
            if (m_saveBundleCallback(m_saveBundleParentDir, m_saveBundleName, &err)) {
                m_saveBundleModalOpen = false;
                m_saveBundleError.clear();
                ImGui::CloseCurrentPopup();
            } else {
                m_saveBundleError = err.empty()
                                        ? std::string("Save failed (see console)")
                                        : err;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_saveBundleModalOpen = false;
            m_saveBundleError.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void WindowManager::setSettingsAppliedCallback(SettingsAppliedCallback cb) {
    m_settingsAppliedCallback = std::move(cb);
    // Forward to the popup; the popup hands the staged copy back through this
    // callback when the user clicks OK.
    m_settingsWindow.setApplyCallback(
        [this](const Settings& s) {
            if (m_settingsAppliedCallback) m_settingsAppliedCallback(s);
        });
    // Wire the OCIO config browse button to our HWND-aware native dialog.
    // Settings modal can't open IFileOpenDialog itself — it doesn't know the
    // owner HWND.
    m_settingsWindow.setBrowseOcioCallback(
        [this]() { return openOcioConfigFileDialog(); });
}

void WindowManager::registerWindow(std::unique_ptr<EditorWindow> window) {
    std::cout << "Registering window: " << window->getName() << std::endl;
    m_windows.push_back(std::move(window));
}

void WindowManager::setWindowVisible(const char* name, bool visible) {
    for (auto& window : m_windows) {
        if (strcmp(window->getName(), name) == 0) {
            window->setVisible(visible);
            return;
        }
    }
}

void WindowManager::resetLayout() {
    std::cout << "Layout reset requested" << std::endl;
    m_layoutResetRequested = true;
    // Clear floating state on layout reset - all windows return to docked state
    m_floatingWindows.clear();
    m_pendingUndock.clear();
    m_pendingDock.clear();
}

void WindowManager::undockWindow(const char* name) {
    std::string windowName(name);
    m_pendingUndock.insert(windowName);
    m_pendingDock.erase(windowName);  // Cancel any pending dock
    std::cout << "Undock requested for: " << name << std::endl;
}

void WindowManager::dockWindow(const char* name) {
    std::string windowName(name);
    m_pendingDock.insert(windowName);
    m_pendingUndock.erase(windowName);  // Cancel any pending undock
    std::cout << "Dock requested for: " << name << std::endl;
}

bool WindowManager::isWindowFloating(const char* name) const {
    return m_floatingWindows.count(std::string(name)) > 0;
}

void WindowManager::renderMenuBar() {
    if (ImGui::BeginMenuBar()) {
        // File menu
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Project...", nullptr)) {
                std::string filePath = openProjectFileDialog();
                if (!filePath.empty() && m_openProjectCallback) {
                    m_openProjectCallback(filePath);
                }
            }

            if (ImGui::MenuItem("Save Project", "Ctrl+S")) {
                if (m_saveProjectCallback) {
                    m_saveProjectCallback();
                }
            }

            if (ImGui::MenuItem("Save Project As...", "Ctrl+Shift+S")) {
                if (m_saveProjectAsCallback) {
                    m_saveProjectAsCallback();
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Writes only the .entity file to the chosen location.\n"
                    "For Managed projects the resulting file is an \"orphan\":\n"
                    "Managed paths are project-relative and won't resolve until\n"
                    "the original project's content/ tree is alongside it.\n"
                    "Use Save As Bundle... to copy the content tree too.");
            }

            if (ImGui::MenuItem("Save Project As Bundle...", nullptr)) {
                m_saveBundleModalOpen = true;
                m_saveBundleModalSeed = true;  // re-seed defaults on next render
                m_saveBundleError.clear();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Creates a portable copy of the project at <parentDir>/<projectName>/:\n"
                    "  - .entity file at the new root\n"
                    "  - content/ + .archive/ + presets/ + objects/ + exports/ + snapshots/\n"
                    ".cache/ is excluded (machine-local, regenerable).\n"
                    "Use this to share a project or move it to external storage.");
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Import Video...", nullptr)) {
                std::string filePath = openVideoFileDialog();
                if (!filePath.empty() && m_videoFileCallback) {
                    m_videoFileCallback(filePath);
                }
            }

            // ADR-0009 — one-shot "make this v6 / mixed-mode project
            // portable" action. Disabled when there's nothing to do.
            const bool canCollect = m_canCollectMediaCallback
                                       ? m_canCollectMediaCallback()
                                       : false;
            if (ImGui::MenuItem("Collect Linked Media into Project Folder...",
                                nullptr, false, canCollect)) {
                if (m_collectMediaCallback) m_collectMediaCallback();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Copies every \"linked\" media reference into the project's\n"
                    "content/unsorted/ folder and flips its entry to \"managed\".\n"
                    "Use this to make a legacy project portable across machines.\n"
                    "Original source files at their previous paths are NOT deleted.");
            }

            // ADR-0009 — orphan-showfile recovery actions. Available when a
            // project is loaded; useful when an .entity was opened at a
            // location that doesn't have its content/ tree (e.g. a bare
            // Save-As copy moved between machines).
            if (ImGui::MenuItem("Rebuild Project Structure",
                                nullptr, false,
                                m_rebuildStructureCallback != nullptr)) {
                if (m_rebuildStructureCallback) {
                    int n = m_rebuildStructureCallback();
                    std::cout << "[WindowManager] Rebuilt " << n
                              << " missing project directories." << std::endl;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Creates any missing canonical project subdirectories\n"
                    "(content/unsorted/, presets/, objects/, exports/,\n"
                    " snapshots/, .cache/thumbnails/). Idempotent — safe\n"
                    "to run on a fully-formed project.");
            }

            if (ImGui::MenuItem("Find Missing Media...",
                                nullptr, false,
                                m_findMissingMediaCallback != nullptr)) {
                std::filesystem::path picked = ui::pickFolderDialog(
                    m_ownerWindow,
                    L"Choose a folder to search for missing media",
                    {});
                if (!picked.empty() && m_findMissingMediaCallback) {
                    int n = m_findMissingMediaCallback(pathToUtf8(picked));
                    std::cout << "[WindowManager] Restored " << n
                              << " missing media files from " << picked.string()
                              << std::endl;
                }
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Recursively searches a folder for media files whose\n"
                    "filenames match Managed entries that are currently\n"
                    "missing from the project's content/ tree. Matches are\n"
                    "copied into the expected canonical paths. Source files\n"
                    "in the search folder are NOT moved.");
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Run Script...", nullptr)) {
                std::string filePath = openScriptFileDialog();
                if (!filePath.empty() && m_runScriptCallback) {
                    m_runScriptCallback(filePath);
                }
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Exit", "Esc")) {
                if (m_exitCallback) {
                    m_exitCallback();
                }
            }

            ImGui::EndMenu();
        }

        // Edit menu — undo/redo + ripple time ops on the active range selection.
        if (ImGui::BeginMenu("Edit")) {
            const bool canUndo = m_canUndoCallback && m_canUndoCallback();
            const bool canRedo = m_canRedoCallback && m_canRedoCallback();
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo)) {
                if (m_undoCallback) m_undoCallback();
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, canRedo)) {
                if (m_redoCallback) m_redoCallback();
            }

            ImGui::Separator();

            const bool hasRange = m_hasRangeSelectionCallback && m_hasRangeSelectionCallback();
            if (ImGui::MenuItem("Insert Time at Selection", "Ctrl+Shift+I", false, hasRange)) {
                if (m_rippleInsertCallback) m_rippleInsertCallback();
            }
            if (ImGui::MenuItem("Remove Selected Time", "Ctrl+Shift+Del", false, hasRange)) {
                if (m_rippleDeleteCallback) m_rippleDeleteCallback();
            }
            if (!hasRange && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Shift+drag on the ruler to select a time range first.");
            }

            ImGui::Separator();

            // Preferences — opens the modal seeded with the live Settings.
            // Disabled until Engine has wired the callbacks (avoids a useless
            // menu entry in degenerate harness configurations).
            const bool prefsReady = static_cast<bool>(m_currentSettingsCallback);
            if (ImGui::MenuItem("Preferences...", nullptr, false, prefsReady)) {
                m_settingsWindow.open(m_currentSettingsCallback());
            }

            ImGui::EndMenu();
        }

        // Windows menu
        if (ImGui::BeginMenu("Windows")) {
            // Window visibility toggles
            for (auto& window : m_windows) {
                bool visible = window->isVisible();
                if (ImGui::MenuItem(window->getName(), nullptr, &visible)) {
                    window->setVisible(visible);
                }
            }

            ImGui::Separator();

            // Dock/Undock submenu
            if (ImGui::BeginMenu("Dock/Undock")) {
                for (auto& window : m_windows) {
                    if (!window->isVisible()) continue;

                    const char* name = window->getName();
                    // Check actual dock state from ImGui
                    ImGuiWindow* imguiWin = ImGui::FindWindowByName(name);
                    bool actuallyFloating = imguiWin && (imguiWin->DockId == 0);

                    if (actuallyFloating) {
                        std::string label = std::string("Dock ") + name;
                        if (ImGui::MenuItem(label.c_str())) {
                            dockWindow(name);
                        }
                    } else {
                        std::string label = std::string("Undock ") + name;
                        if (ImGui::MenuItem(label.c_str())) {
                            undockWindow(name);
                        }
                    }
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            // Layout lock toggle
            if (ImGui::MenuItem(m_layoutLocked ? "Unlock Layout" : "Lock Layout", "Ctrl+L")) {
                m_layoutLocked = !m_layoutLocked;
                std::cout << "Layout " << (m_layoutLocked ? "locked" : "unlocked") << std::endl;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(m_layoutLocked ?
                    "Unlock layout to rearrange docked windows" :
                    "Lock layout to prevent accidental undocking");
            }

            // Layout reset
            if (ImGui::MenuItem("Reset Layout")) {
                resetLayout();
            }

            ImGui::EndMenu();
        }

        // Help menu
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::BeginMenu("Keyboard Shortcuts")) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Playback:");
                ImGui::BulletText("Space        - Play/Pause");
                ImGui::BulletText("J            - Step backward (1 frame)");
                ImGui::BulletText("K            - Pause");
                ImGui::BulletText("L            - Step forward (1 frame)");
                ImGui::BulletText("Left Arrow   - Step backward (1 frame)");
                ImGui::BulletText("Right Arrow  - Step forward (1 frame)");
                ImGui::BulletText("Home         - Go to start");
                ImGui::BulletText("End          - Go to end");

                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Timeline Editing:");
                ImGui::BulletText("S            - Split clip at playhead");
                ImGui::BulletText("Delete       - Delete selected clip");
                ImGui::BulletText("Ctrl+D       - Duplicate selected clip");
                ImGui::BulletText("Drag clip    - Move horizontally or to another track");
                ImGui::BulletText("Drag edges   - Trim in/out points");

                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Project:");
                ImGui::BulletText("Ctrl+S       - Save project");
                ImGui::BulletText("Ctrl+Shift+S - Save project as...");
                ImGui::BulletText("Ctrl+O       - Open project");

                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Timeline View:");
                ImGui::BulletText("Alt+Scroll   - Zoom in/out");

                ImGui::Separator();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Window Management:");
                ImGui::BulletText("Right-click title bar - Undock/Dock window");
                ImGui::BulletText("Ctrl+L       - Toggle layout lock");
                ImGui::BulletText("Windows > Dock/Undock - Undock specific windows");
                ImGui::BulletText("(Layout locked by default - prevents drag-undocking)");
                ImGui::BulletText("When unlocked: drag tab bar to rearrange");

                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem("About Entity Media Server")) {
                // Could open an about dialog in the future
            }

            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }
}

void WindowManager::createDefaultLayout(ImGuiID dockspaceId) {
    std::cout << "Creating default window layout..." << std::endl;

    // Clear any existing layout
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    // Split the dockspace into regions:
    // - Bottom 30% for Timeline
    // - Top 70% split: Left 20% for Media Bin, Center for Stage, Right 20% for Properties

    ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Down, 0.3f, nullptr, &dockspaceId);
    ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.2f, nullptr, &dockspaceId);
    ImGuiID dockRight = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Right, 0.25f, nullptr, &dockspaceId);
    ImGuiID dockCenter = dockspaceId;  // Remaining center area is for Stage

    // Dock windows to their designated nodes
    // Note: Last window docked to a node becomes the active tab
    ImGui::DockBuilderDockWindow("Timeline", dockBottom);

    // Left panel: Media Bin, Model Bin, Screens as tabs (Media Bin = active)
    ImGui::DockBuilderDockWindow("Screens", dockLeft);
    ImGui::DockBuilderDockWindow("Model Bin", dockLeft);
    ImGui::DockBuilderDockWindow("Media Bin", dockLeft);  // Docked last = active tab

    // Center: Stage and Mapping as tabs (Stage = active)
    ImGui::DockBuilderDockWindow("Mapping", dockCenter);
    ImGui::DockBuilderDockWindow("Stage", dockCenter);

    ImGui::DockBuilderDockWindow("Properties", dockRight);

    ImGui::DockBuilderFinish(dockspaceId);

    std::cout << "Default layout created: Timeline (bottom), Media Bin/Model Bin/Screens (left tabs), Stage/Mapping (center tabs), Properties (right)" << std::endl;
}

std::string WindowManager::openVideoFileDialog() {
    auto path = ui::openFileDialog(
        m_ownerWindow,
        L"Import Video",
        {
            {L"Video Files",   L"*.mov;*.mp4;*.avi;*.mkv;*.webm"},
            {L"PNG Sequence",  L"*.png"},
            {L"All Files",     L"*.*"},
        });
    if (path.empty()) return "";
    std::string utf8 = pathToUtf8(path);
    std::cout << "Selected file: " << utf8 << std::endl;
    return utf8;
}

std::string WindowManager::openProjectFileDialog() {
    auto path = ui::openFileDialog(
        m_ownerWindow,
        L"Open Project",
        {
            {L"Entity Project Files", L"*.entity"},
            {L"All Files",            L"*.*"},
        });
    if (path.empty()) return "";
    std::string utf8 = pathToUtf8(path);
    std::cout << "Selected project file: " << utf8 << std::endl;
    return utf8;
}

std::string WindowManager::saveProjectFileDialog(const std::string& suggestedPath) {
    std::filesystem::path suggestion = suggestedPath.empty()
                                           ? std::filesystem::path{}
                                           : std::filesystem::path(suggestedPath);
    auto path = ui::saveFileDialog(
        m_ownerWindow,
        L"Save Project As",
        {
            {L"Entity Project Files", L"*.entity"},
            {L"All Files",            L"*.*"},
        },
        L"entity",
        suggestion);
    if (path.empty()) return "";
    std::string utf8 = pathToUtf8(path);
    std::cout << "Saving project to: " << utf8 << std::endl;
    return utf8;
}

std::string WindowManager::openScriptFileDialog() {
    auto path = ui::openFileDialog(
        m_ownerWindow,
        L"Run Script",
        {
            {L"JSON Script Files", L"*.json"},
            {L"All Files",         L"*.*"},
        });
    if (path.empty()) return "";
    std::string utf8 = pathToUtf8(path);
    std::cout << "Selected script file: " << utf8 << std::endl;
    return utf8;
}

std::string WindowManager::openOBJFileDialog() {
    auto path = ui::openFileDialog(
        m_ownerWindow,
        L"Import OBJ Model",
        {
            {L"OBJ Model Files", L"*.obj"},
            {L"All Files",       L"*.*"},
        });
    if (path.empty()) return "";
    std::string utf8 = pathToUtf8(path);
    std::cout << "Selected OBJ file: " << utf8 << std::endl;
    return utf8;
}

std::string WindowManager::openOcioConfigFileDialog() {
    auto path = ui::openFileDialog(
        m_ownerWindow,
        L"Choose OCIO Config",
        {
            {L"OCIO Config Files", L"*.ocio"},
            {L"All Files",         L"*.*"},
        });
    if (path.empty()) return "";
    std::string utf8 = pathToUtf8(path);
    std::cout << "Selected OCIO config: " << utf8 << std::endl;
    return utf8;
}

} // namespace entity
