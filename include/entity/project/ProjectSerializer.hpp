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
    //   v15 (2026-05-11) — per-track "layers[]" array replaces "clips[]".
    //                      Each entry carries a "kind" field ("clip" |
    //                      "object_animation") plus kind-specific payload.
    //                      ObjectAnimation entries persist: target (by Screen
    //                      or Prop name), startFrame, duration,
    //                      sectionBehavior, layer name/color, and
    //                      AnimatedProperties keyframe tracks. v14 "clips[]"
    //                      entries load as kind="clip" with no migration
    //                      step required — the loaders accept both keys.
    //   v14 (2026-05-11) — Screen/Prop transform expresses real-world size
    //                      in meters via a new "size" array, replacing the
    //                      unitless "scale" key. v≤13 files load by reading
    //                      "scale" and computing size = legacyScale × mesh
    //                      native bounds after the model is resolved, so the
    //                      rendered dimensions are byte-identical to the
    //                      pre-migration result. First save after upgrade
    //                      emits "size" only.
    //   v13 (2026-05-09) — adds Prop array. Each entry persists name,
    //                      transform, visibility, displayColor, modelName.
    //                      Props are pre-vis-only stage geometry (see
    //                      Prop.hpp); excluded from SceneSnapshot and the
    //                      projector output path. Older files load with
    //                      no props (missing array = empty); first save
    //                      after upgrade emits the array, even if empty.
    //   v11 (2026-05-07) — adds Clip.mediaOutFrame (INCLUSIVE last frame
    //                      played, industry convention), decoupling the
    //                      source-playback out-point from the timeline-
    //                      footprint duration. v10 clips load with the
    //                      out-point derived from
    //                      `mediaStartFrame + duration*ratio - 1` (clamped
    //                      to totalMediaFrames - 1), reproducing the old
    //                      implicit boundary exactly so playback is
    //                      unchanged.
    //   v12 (2026-05-08) — persists per-media-entry probe cache
    //                       (lastProbeSizeBytes / lastProbeMtimeUnix /
    //                       probe sub-object). Lets reopen skip the
    //                       FFmpeg metadata probe when the file's
    //                       size + mtime match the saved snapshot.
    //                       Older files load fine — missing fields
    //                       default to 0 / invalid, forcing a one-time
    //                       re-probe that the next save persists.
    //   v10 (2026-05-07) — drops Section.name. v9 entries kept a "name"
    //                      string per section; the loader now silently
    //                      ignores it (no separate migration step needed —
    //                      `j.value(...)` already tolerates missing keys
    //                      and unknown keys are ignored).
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
    // v8 files load with no cues (missing "cues" key = empty);
    // v11 mediaLibrary entries load with no probe cache (re-index on first open);
    // v14 per-track "clips[]" entries load as kind="clip" (no migration step);
    // v15 layers[] kind discriminator;
    // v16 per-clip "effects[]" array (issue #54). Pre-v16 clips load with
    //     no effect chain attached.
    static constexpr int PROJECT_VERSION = 16;
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
