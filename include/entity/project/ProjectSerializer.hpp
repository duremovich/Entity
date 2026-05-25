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
    // v17 ObjectAnimationLayer "endBehavior" ("Hold" | "Reset"). Pre-v17 OA
    //     layers load with the new "Hold" default per ADR-0020 — the
    //     explicit show-friendly choice, not pre-ADR snap-back.
    // v18 objectLibrary[] — parallel to mediaLibrary[] but for 3D models in
    //     `objects/`. Persists pathKind + size validity gate so the
    //     scanner can hot-reload changed meshes. Pre-v18 projects load
    //     with an empty object library; existing Model.filepath entries
    //     keep working as Linked paths.
    // v19 Prop "opacity" — stage-view-only fade for props, matching the
    //     existing Screen.opacity. Pre-v19 props default to opacity=1.0.
    //     Screen.opacity was already persisted; only the wiring into the
    //     stage view is new in this version.
    // v20 ContentRouting library (ADR-0022). Promotes per-layer inline
    //     `contentRouting` to a top-level `contentRoutingAssets` array.
    //     Each Clip / GenerativeLayer carries a `contentRoutingAssetName`
    //     reference (named lookup so the link survives entity-ID churn
    //     across reloads). Pre-v20 projects migrate transparently: each
    //     legacy single-target Direct routing resolves to the Screen's
    //     auto-direct asset; multi-target routings spin up new
    //     "Custom Routing N" assets and the layer points at them.
    // v21 Generative layer persistence. Adds kind="generative" entries in
    //     layers[]. Each entry carries GenerativeLayer (renderWidth/Height,
    //     targetScreenName), MediaLayer, Transform (same as Clip), and an
    //     optional `text_state` object for Text sub-kind layers. Pre-v21
    //     projects load without generative layers (they were silently
    //     skipped on save in v20 and earlier — no data loss, just re-create
    //     from scratch). v21 files load with default-constructed TextLayerState
    //     if `text_state` is absent on a generative layer.
    // v22 DMX per-project mapping table (#13 / #59). New top-level
    //     "dmxMappingsJson" string field carrying the project-scoped DMX
    //     channel mapping table as raw JSON. The dmx-artnet plugin reads
    //     this through IPluginContext::getStringSetting("dmxMappingsJson")
    //     and falls back to its baked default mappings when the string is
    //     empty (which is the default for pre-v22 files). Stored as a
    //     string rather than a parsed object because the typed DmxMapping
    //     struct lives in the Apache-2.0 plugin headers and round-tripping
    //     through a typed schema in the GPL serializer would create the
    //     same boundary problem (see ADR-0024).
    // v23: per-clip AudioSource (gain/mute/solo); project master gain/mute.
    // v24: section breaks + cue tags stored as integer timeline frames
    //      (was microseconds). Loader migrates pre-v24 files via timeToFrame.
    // v25: per-layer LayerTrackUiState (timeline twirl-down collapsed
    //      group paths). Pre-v25 files load with empty state — all
    //      groups expanded. Editor-only; never reaches the bus.
    static constexpr int PROJECT_VERSION = 25;
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
