#pragma once

#include "entity/ui/Workspace.hpp"

#include <filesystem>
#include <vector>

namespace entity {

/**
 * WorkspaceStore — disk persistence for the named-workspace list.
 *
 * Mirrors the Settings.cpp pattern: lives at `%APPDATA%/Entity/workspaces.json`,
 * defensive load (corrupt file falls back to defaults), atomic write via
 * temp + rename. WindowManager owns the in-memory list and writes through
 * to this on save / shutdown.
 */
namespace WorkspaceStore {

/**
 * Resolve the absolute path to `workspaces.json` for the current
 * platform/user. The directory may not exist yet; saveAll() creates it.
 */
std::filesystem::path workspacesPath();

/**
 * Load all workspaces from disk. Returns at minimum the built-in
 * "Default" entry (empty imguiIni, builtIn=true) when the file is missing
 * or unreadable. Never throws; corrupt JSON is logged and treated as
 * "first run" so a busted file doesn't brick the editor.
 */
std::vector<Workspace> loadAll();

/** Same, reads from a specific path (used by tests). */
std::vector<Workspace> loadAll(const std::filesystem::path& path);

/**
 * Persist the workspace list to disk. Creates the parent directory if
 * needed. Returns false on I/O failure (and leaves any pre-existing file
 * intact via temp-file + rename).
 */
bool saveAll(const std::vector<Workspace>& workspaces);

/** Same, writes to a specific path (used by tests). */
bool saveAll(const std::vector<Workspace>& workspaces,
             const std::filesystem::path& path);

/** Returns a default-constructed "Default" workspace (builtIn=true, empty ini). */
Workspace defaultWorkspace();

} // namespace WorkspaceStore

} // namespace entity
