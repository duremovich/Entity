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
    //   v9 (2026-05-06) — adds Timeline cue tags array (Phase A — numbered
    //                     timeline markers fired by command). Each entry is
    //                     {number, timestamp, label}. v8 files load with no
    //                     cues (missing "cues" key = empty array).
    //   v8 (2026-05-03) — adds Projector array + per-projector calibration
    //                     state (pose, FOV, k1/k2 lens distortion,
    //                     calibrationPoints vector, linkedOutput by
    //                     OutputDisplay name, targetSurfaces by Screen
    //                     name, useResidualWarp opt-in). See ADR-0011 for
    //                     the calibration system. v7 files load with no
    //                     projectors (missing array = none); the user
    //                     re-creates them via ScreensWindow.
    //   v7 (2026-04-29) — ADR-0009 structured projects. Adds per-
    //                     mediaLibrary-entry `pathKind` ("managed" |
    //                     "linked") plus optional `archivedOriginal`
    //                     and `originalCodec` for the archive-on-
    //                     transcode flow. v6 entries load with
    //                     pathKind="linked" (matches pre-v7 behavior:
    //                     absolute paths everywhere) and no archive.
    //   v6 (2026-04-28) — adds OutputDisplay.ocioDisplay + ocioView (Phase C.12 #8)
    //                     and (in #9) the optional MediaLibraryEntry
    //                     `inputColorSpaceOverride` field. The override is
    //                     emitted only when non-empty, so older v6 files
    //                     without it still load cleanly — no separate version
    //                     bump.
    //   v5 (2026-04-24) — replaces autoTranscodeOnImport with nonHapImportPolicy.
    //   v4 — adds mediaLibrary array + autoTranscodeOnImport.
    //   v3 — adds Timeline::Section array.
    //   v2 — adds OutputDisplay + Models + Screens.
    //   v1 — original.
    // Loader is forward-compatible with older files: missing arrays no-op;
    // v4 autoTranscodeOnImport is translated to AlwaysTranscode/NeverTranscode;
    // v5 outputs load with empty OCIO display/view (= config default at draw time);
    // v6 mediaLibrary entries load with pathKind="linked";
    // v7 files load with no projectors (missing "projectors" key = empty);
    // v8 files load with no cues (missing "cues" key = empty).
    static constexpr int PROJECT_VERSION = 9;
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
