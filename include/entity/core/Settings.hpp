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

    // -----------------------------------------------------------------
    // Phase C.12 #7 — OCIO color management.
    //
    // Empty `ocioConfigPath` = use the bundled ACES Studio Config 1.3 (OCIO
    // 2.4+ ships it as a built-in resource — no on-disk file needed). A
    // non-empty path loads the user's custom .ocio config via
    // CreateFromFile. C.12 ships restart-required hot-swap; the editor
    // notices the new path on next launch.
    //
    // The default-* fields are *fallback* values consulted when a decoder
    // can't determine the source color space from the codec. Always
    // overridden by explicit metadata: ProRes' AVCOL_SPC_*, HAP variant
    // tags, etc. PNG always consults defaultPngInputCs because the PNG
    // spec doesn't carry an OCIO color-space name — only an EOTF.
    //
    // The two "default*" output fields are *initial* values for the next
    // OutputDisplay the user creates; per-output overrides land in C.12 #8
    // (project-persistent display+view per OutputDisplay). Empty = OCIO
    // config's own default display/view.
    // -----------------------------------------------------------------
    std::string ocioConfigPath;
    std::string defaultVideoInputCs{"Linear Rec.709 (sRGB)"};
    std::string defaultPngInputCs{"sRGB - Display"};
    std::string defaultDisplay;
    std::string defaultView;

    // -----------------------------------------------------------------
    // #32 — per-import storage decision (Copy into project vs. Link to
    // original). Default Ask: the unified import modal pops on every
    // import and offers "Don't ask again" toggles that flip this to
    // AlwaysCopy / AlwaysLink. Per-machine, not per-project — different
    // workstations may want different defaults (touring rig vs. authoring
    // box).
    // -----------------------------------------------------------------
    enum class ImportStoragePolicy : uint8_t {
        Ask         = 0,
        AlwaysCopy  = 1,
        AlwaysLink  = 2,
    };
    ImportStoragePolicy importStoragePolicy{ImportStoragePolicy::Ask};

    // -----------------------------------------------------------------
    // OSC receiver plugin. Read once at plugin register time
    // (Engine::initialize loads + publishes settings before
    // registerStaticPlugins runs). Restart-to-apply, mirroring
    // ocioConfigPath's lifecycle. ENV var ENTITY_OSC_PORT still wins
    // when set, as a developer escape hatch.
    // -----------------------------------------------------------------
    bool     oscReceiverEnabled{true};
    uint16_t oscReceiverPort{53000};

    // -----------------------------------------------------------------
    // DMX / Art-Net / sACN / Enttec plugin (Phase D, #13).
    //
    // Every surface is opt-in -- new I/O surfaces default off so
    // installing the build doesn't unexpectedly bind ports. Editing
    // any of these requires an editor restart; the plugin reads them
    // once at register time.
    //
    // Per-project DMX channel mappings live in the .entity file
    // (schema v22+, Phase 5), NOT in Settings. The plugin's baked
    // default mapping table covers transport + cues 1-4 on universe 0
    // out-of-box.
    // -----------------------------------------------------------------
    bool        dmxArtnetEnabled{false};
    uint16_t    dmxArtnetListenPort{6454};
    bool        dmxSacnEnabled{false};
    bool        dmxOutEnabled{false};
    std::string dmxOutArtnetTargets;        // comma-separated dotted-quad IPs; empty -> broadcast
    bool        dmxOutSacnEnabled{false};
    bool        dmxEnttecEnabled{false};
    std::string dmxEnttecPort;              // e.g. "COM5"; empty -> auto-pick first FTDI device
    uint16_t    dmxEnttecUniverse{0};

    // -----------------------------------------------------------------
    // Active named workspace (UI dock layout). The workspaces themselves
    // live in `%APPDATA%/Entity/workspaces.json` (see WorkspaceStore);
    // this field is just the "currently selected" pointer. Survives app
    // restarts so the user reopens into whichever workspace they last
    // had active.
    // -----------------------------------------------------------------
    std::string activeWorkspace{"Default"};
};

/**
 * Process-wide Settings snapshot accessor.
 *
 * Decoders consult this from worker threads to read user defaults like
 * `defaultPngInputCs` without having a live Settings reference plumbed
 * through every constructor / factory call. Thread-safe; returns a copy
 * (the strings are short and the call is rare — once per decoded frame at
 * worst).
 *
 * Engine calls `publishActiveSettings()` once at startup after loadSettings()
 * and again whenever the Preferences modal applies a change.
 */
Settings activeSettings();
void publishActiveSettings(const Settings& s);

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
