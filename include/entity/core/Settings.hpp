#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace entity {

/**
 * Settings — machine-global Entity preferences (not per-project).
 *
 * These are values whose "right answer" is hardware- or user-dependent, not
 * show-dependent: how much RAM to dedicate to the frame cache, where to put
 * the autosave directory, the user's preferred default transcode policy.
 * Per-project values (timeline FPS, screen layout, clip choices) live in the
 * .entity file and are owned by ProjectSerializer; this is the other side of
 * that line.
 *
 * Storage location:
 *   - Windows:  %APPDATA%/Entity/settings.json
 *   - macOS:    ~/Library/Application Support/Entity/settings.json (TODO)
 *   - Linux:    ~/.config/Entity/settings.json (TODO)
 *
 * On first run the file doesn't exist; loadSettings() returns the default-
 * constructed Settings. saveSettings() creates the parent directory if needed.
 *
 * Adding a new setting:
 *   1. Add a field with a default initializer below.
 *   2. Add a key in Settings.cpp's serialize/deserialize blocks (preserve
 *      existing values — older config files must still load cleanly).
 *   3. Surface it in the Preferences dialog (src/ui/SettingsWindow.cpp).
 */
struct Settings {
    // Total RAM budget for the (forthcoming Phase C.10) FrameCache, in bytes.
    // 512 MB matches Disguise/Pixera-class behavior and gives ~128 frames of
    // 4K HAP-Q headroom (~4 MB/frame). Hardware-dependent — bump on a 64 GB
    // workstation, drop on a laptop. Not yet wired to a cache (the cache
    // lands in C.10); persisted now so the value survives across that work.
    uint64_t frameCacheBytes{512ull * 1024ull * 1024ull};
};

/**
 * Resolve the absolute path to `settings.json` for the current platform/user.
 * The directory may not exist yet; saveSettings() creates it on demand.
 */
std::filesystem::path settingsPath();

/**
 * Load settings from disk, or return defaults if the file doesn't exist or
 * is unreadable. Never throws — corrupt JSON is logged and treated as
 * "use defaults" so a busted settings.json can't brick the editor.
 */
Settings loadSettings();

/** Same, but reads from a specific path (used by tests). */
Settings loadSettings(const std::filesystem::path& path);

/**
 * Persist settings to disk. Creates the parent directory if needed. Returns
 * false on I/O failure (and leaves any pre-existing file intact — writes via
 * a temp-file + rename to avoid half-written state on crash).
 */
bool saveSettings(const Settings& settings);

/** Same, but writes to a specific path (used by tests). */
bool saveSettings(const Settings& settings, const std::filesystem::path& path);

} // namespace entity
