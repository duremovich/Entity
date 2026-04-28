#pragma once

#include "entity/timeline/Timeline.hpp"
#include <string>
#include <filesystem>

namespace entity {

class ProjectManager;  // forward-declared; optional save/load argument

/**
 * ProjectSerializer - Save and load Entity project files.
 *
 * Serializes timeline state, tracks, and clips to JSON format.
 * Runtime resources (textures, decoders) are NOT serialized;
 * they are recreated when a project is loaded.
 *
 * File format: .entity (JSON)
 */
class ProjectSerializer {
public:
    // Project file version for forward compatibility.
    //   v6 (2026-04-28) — adds OutputDisplay.ocioDisplay + ocioView (Phase C.12 #8).
    //   v5 (2026-04-24) — replaces autoTranscodeOnImport with nonHapImportPolicy.
    //   v4 — adds mediaLibrary array + autoTranscodeOnImport.
    //   v3 — adds Timeline::Section array.
    //   v2 — adds OutputDisplay + Models + Screens.
    //   v1 — original.
    // Loader is forward-compatible with older files: missing arrays no-op;
    // v4 autoTranscodeOnImport is translated to AlwaysTranscode/NeverTranscode;
    // v5 outputs load with empty OCIO display/view (= config default at draw time).
    static constexpr int PROJECT_VERSION = 6;
    static constexpr const char* FILE_EXTENSION = ".entity";

    /**
     * Save a timeline to a project file.
     * @param timeline  Timeline to save
     * @param filepath  Path to save to (will append .entity if needed)
     * @param projectMgr Optional; if non-null, its media library +
     *                   auto-transcode flag are serialized. Pass nullptr
     *                   when only the timeline portion matters.
     */
    static bool save(const Timeline& timeline, const std::filesystem::path& filepath,
                     const ProjectManager* projectMgr = nullptr);

    /**
     * Load a project file into a timeline.
     * Clears existing timeline content before loading.
     * @param timeline           Timeline to load into
     * @param filepath           Project file path
     * @param mediaLoadCallback  Per-clip media-open callback
     * @param projectMgr         Optional; if non-null, populated with the
     *                           saved media library + auto-transcode flag.
     */
    using MediaLoadCallback = std::function<void(entt::entity clipEntity, const std::string& filepath)>;
    static bool load(Timeline& timeline, const std::filesystem::path& filepath,
                     MediaLoadCallback mediaLoadCallback = nullptr,
                     ProjectManager* projectMgr = nullptr);

    /**
     * Get the last error message (if save/load failed).
     */
    static const std::string& getLastError() { return s_lastError; }

private:
    static std::string s_lastError;
};

} // namespace entity
