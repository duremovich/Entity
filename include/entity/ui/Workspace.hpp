#pragma once

#include <string>
#include <vector>

namespace entity {

/**
 * Workspace — a named arrangement of editor windows.
 *
 * Stores the ImGui dock layout (Windows + Docking ini sections), the layout
 * lock state, and the list of windows currently toggled off via the Windows
 * menu. Switching workspaces feeds `imguiIni` into
 * `ImGui::LoadIniSettingsFromMemory` and applies the lock/visibility state
 * to WindowManager.
 *
 * Persisted to `%APPDATA%/Entity/workspaces.json` via WorkspaceStore.
 */
struct Workspace {
    // Unique id and display label. "Default" is reserved for the built-in
    // workspace and cannot be deleted or renamed.
    std::string name;

    // Full content of an ImGui ini file (Window placement + Docking tree).
    // Empty means "no saved layout yet — use procedural defaults" — the
    // built-in Default workspace ships empty so first-run users hit
    // WindowManager::createDefaultLayout(), then their tweaks get captured
    // here on save.
    std::string imguiIni;

    // Layout-lock state at the moment the workspace was saved. Restored
    // when switching to this workspace.
    bool layoutLocked = false;

    // Names of windows toggled off via the Windows menu. On switch,
    // WindowManager calls setWindowVisible(name, false) for each.
    std::vector<std::string> hiddenWindows;

    // Per-dock-node active tab window names, captured background-to-foreground
    // (globally-focused window last). Restored via SetWindowFocus after the
    // ini is applied: the ini already encodes SelectedTabId, but the
    // NavWindow-follows write in DockNodeUpdateTabBar can stomp it on the
    // rebuild frame, so this explicit focus pass is what makes a workspace
    // switch land on the same tabs it was saved with. Empty for pre-Phase-11
    // workspaces (the focus pass no-ops, the ini's selection stands).
    std::vector<std::string> focusedTabs;

    // True for the shipped "Default" entry. Built-in workspaces can be
    // saved-into and reset, but not deleted.
    bool builtIn = false;
};

} // namespace entity
