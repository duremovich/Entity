#pragma once

/**
 * ProjectManager - owns project-scoped state: the current project file path,
 * the media-library list, and the autosave timer. Wraps ProjectSerializer
 * with the Engine-level orchestration (what to clear before load, what to
 * emplace after, how often to autosave).
 *
 * Per-clip decode state used to live here conceptually (as a std::unordered_map
 * on Engine); it has since moved to the ClipDecodeState component. What's left
 * is legitimately project-wide: one path, one media list, one autosave cadence.
 *
 * Ownership: Engine holds a std::unique_ptr<ProjectManager>. ProjectManager
 * holds non-owning pointers to Timeline, registry, and IRenderer — all outlive
 * it because Engine owns them too.
 */

#include "entity/core/Types.hpp"
#include <entt/entt.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace entity {

class Timeline;
class IRenderer;

class ProjectManager {
public:
    ProjectManager() = default;
    ~ProjectManager() = default;

    ProjectManager(const ProjectManager&) = delete;
    ProjectManager& operator=(const ProjectManager&) = delete;

    // ADR-0009 — canonical subdirectory names for the structured-
    // project layout. Exposed as constants so the launcher, import
    // flow, transcode/archive logic, and tests all reference the
    // same strings without fanning out as "content"/"presets"/...
    // string literals. Matches the tree createNew() builds on disk.
    static constexpr const char* kContentDir    = "content";
    static constexpr const char* kUnsortedDir   = "unsorted";    // under content/
    static constexpr const char* kArchiveDir    = ".archive";    // sibling of files in any content/<sub>/
    static constexpr const char* kPresetsDir    = "presets";
    static constexpr const char* kObjectsDir    = "objects";
    static constexpr const char* kExportsDir    = "exports";
    static constexpr const char* kSnapshotsDir  = "snapshots";
    static constexpr const char* kCacheDir      = ".cache";
    static constexpr const char* kThumbnailsDir = "thumbnails";  // under .cache/

    /**
     * Attach the dependencies ProjectManager needs. Called by Engine after
     * it has constructed all three. Safe to call once; don't re-initialize.
     */
    void initialize(Timeline* timeline, entt::registry* registry, IRenderer* renderer);

    // --- Save / load ---------------------------------------------------------

    /**
     * Create a new structured project (ADR-0009) on disk. Builds the
     * subdirectory tree under `parentDir/projectName/`, writes an empty
     * `<projectName>.entity` at the root, sets `m_projectPath` to that
     * file, and clears the in-memory media library so the new project
     * starts from a clean slate.
     *
     * Layout produced:
     *   <parentDir>/<projectName>/
     *     ├── <projectName>.entity     (empty v7 project)
     *     ├── content/unsorted/        (default landing zone for imports)
     *     ├── presets/
     *     ├── objects/
     *     ├── exports/
     *     ├── snapshots/
     *     └── .cache/thumbnails/
     *
     * Fails (returns false, leaves state unchanged) if:
     *   - `projectName` is empty or contains a path separator
     *   - `<parentDir>/<projectName>` already exists on disk
     *   - the filesystem rejects directory creation or file write
     *
     * Note: callable before Engine attaches a Timeline. The empty
     * project is written by routing through ProjectSerializer with a
     * temporary Timeline, so the schema version automatically tracks
     * `ProjectSerializer::PROJECT_VERSION` rather than hand-rolled JSON.
     */
    bool createNew(const std::filesystem::path& parentDir,
                   const std::string& projectName);

    /**
     * Save the current timeline to `filepath`. Empty path uses the currently
     * set project path, or "project.entity" if nothing has been saved yet.
     * Updates the current project path on success.
     */
    bool save(const std::filesystem::path& filepath = "");

    /**
     * Load a project file, clearing and re-populating clip decode state via
     * the renderer. Updates the current project path on success.
     */
    bool load(const std::filesystem::path& filepath);

    /**
     * Periodic autosave. Accumulates deltaTime and, once it crosses the
     * configured interval, writes the current timeline to
     *   <projectPath>.autosave   (or autosave.entity if no project is set).
     * Does not touch the "real" project path — autosave is a side channel.
     */
    void tickAutosave(double deltaTime);

    void   setAutosaveInterval(double seconds) { m_autosaveInterval = seconds; }
    double autosaveInterval() const            { return m_autosaveInterval; }

    // --- Project path --------------------------------------------------------

    const std::filesystem::path& projectPath() const { return m_projectPath; }
    void setProjectPath(const std::filesystem::path& p) { m_projectPath = p; }

    // --- Media library -------------------------------------------------------

    /**
     * Per ADR-0009. `Managed` = path is project-relative (under
     * `content/...`), resolved against the project root. `Linked` =
     * path is absolute and used as-is (the QLab/Watchout escape
     * hatch).
     *
     * v6 → v7 migration sets every existing entry to Linked since the
     * old format only stored absolute paths. New imports default to
     * Managed once the structured-import flow lands.
     */
    enum class PathKind : uint8_t {
        Managed = 0,
        Linked  = 1,
    };

    /**
     * One entry per imported source file. `originalPath` is the
     * canonical identity of the clip's source as stored on disk.
     * For `Linked` entries it's an absolute path; for `Managed`
     * entries it's project-relative (e.g. `content/act1/intro.mov`).
     *
     * `transcodedPath` is the HAP variant the TranscodeManager
     * produced, empty until the worker finishes (or forever if auto-
     * transcode was off). `variant` records which HAP variant was
     * written so a re-transcode with a different variant doesn't
     * silently overwrite. When schema v7's archive-on-transcode flow
     * is wired up (tracked in the structured-projects epic), the
     * transcode replaces the source at the canonical path and
     * `transcodedPath` becomes legacy / equal to `originalPath`.
     *
     * When the decoder opens a clip, it calls
     * `decoderPathFor(clip.filepath)` to pick between original and
     * transcoded.
     */
    struct MediaLibraryEntry {
        std::string originalPath;
        std::string transcodedPath;   // empty if not transcoded (yet)
        std::string variant;          // e.g. "hap_alpha"; empty if transcoded empty

        // Phase C.12 #9 — optional per-clip OCIO input color-space override.
        // Empty = "Auto (decoder)" — let the codec/decoder tag drive the OCIO
        // input transform (HAP RGB → "Linear Rec.709 (sRGB)", ProRes via
        // AVColorSpace, PNG via Settings.defaultPngInputCs, etc.). Non-empty
        // forces the named OCIO color space for every clip backed by this
        // media entry, overriding the decoder tag at upload time. Persisted
        // in the project file only when non-empty.
        std::string inputColorSpaceOverride;

        // ADR-0009 — managed vs. linked path semantics. Default Linked
        // matches pre-v7 behavior (absolute paths everywhere). The
        // structured-import flow flips new imports to Managed.
        PathKind pathKind{PathKind::Linked};

        // ADR-0009 — when the source was transcoded under the
        // archive-on-transcode flow, `archivedOriginal` is the
        // project-relative path to the preserved pre-transcode source
        // (e.g. `content/act1/.archive/intro.mov`). Empty when no
        // archive exists (file was imported as HAP-already, or was
        // never transcoded). Persisted only when non-empty.
        std::string archivedOriginal;

        // ADR-0009 — the codec the source carried before transcode
        // (e.g. "prores4444", "h264"). Used by the MediaBin "Restore
        // Original" UX to label the archived entry. Empty when no
        // archive exists. Persisted only when non-empty.
        std::string originalCodec;
    };

    /**
     * Register `originalPath`. Idempotent — if an entry exists it is not
     * touched. Returns a reference to the (possibly-existing) entry.
     */
    MediaLibraryEntry& addMediaFile(const std::string& originalPath);

    /**
     * Update (or create) the transcoded side of the entry. Used by Engine
     * when a TranscodeManager worker flips to Done.
     */
    void setTranscodedPath(const std::string& originalPath,
                           const std::string& transcodedPath,
                           const std::string& variant);

    /**
     * Find an entry by original path. Returns nullptr if none exists.
     */
    const MediaLibraryEntry* findEntry(const std::string& originalPath) const;
    MediaLibraryEntry*       findEntry(const std::string& originalPath);

    /**
     * Remove the entry. Caller is responsible for any in-flight transcode
     * worker.
     */
    void removeMediaFile(const std::string& originalPath);

    /**
     * Resolve which file path the decoder should actually open for a clip
     * whose stored `filepath` is `originalPath`. Returns the transcoded
     * path when available and on-disk, else the original.
     */
    std::string decoderPathFor(const std::string& originalPath) const;

    const std::vector<MediaLibraryEntry>& loadedMediaFiles() const { return m_loadedMediaFiles; }

    // --- Import preferences -------------------------------------------------

    /**
     * What to do with a non-HAP source file on import. Persisted with the
     * project. User-toggleable via the MediaBin toolbar combo; the
     * first-import modal's "Don't ask again" checkbox also writes here.
     *
     * Ask (default): stash a pending decision on Engine + show a modal
     * the next MediaBin render. Until the user picks, nothing is added
     * to the timeline.
     *
     * AlwaysTranscode: silently enqueue a HAP transcode. Used to be
     * behavior of `autoTranscodeOnImport = true`.
     *
     * NeverTranscode: silently create a clip on the source. Slow for
     * ProRes 4K but matches what the user asked for.
     */
    enum class NonHapImportPolicy : uint8_t {
        Ask = 0,
        AlwaysTranscode = 1,
        NeverTranscode = 2,
    };

    NonHapImportPolicy nonHapImportPolicy() const         { return m_nonHapImportPolicy; }
    void setNonHapImportPolicy(NonHapImportPolicy policy) { m_nonHapImportPolicy = policy; }

private:
    // Non-owning dependencies (Engine owns and outlives this)
    Timeline*        m_timeline{nullptr};
    entt::registry*  m_registry{nullptr};
    IRenderer*       m_renderer{nullptr};

    std::filesystem::path           m_projectPath;
    std::vector<MediaLibraryEntry>  m_loadedMediaFiles;
    NonHapImportPolicy              m_nonHapImportPolicy{NonHapImportPolicy::Ask};

    double m_autosaveInterval{30.0};
    double m_autosaveAccumulator{0.0};
};

} // namespace entity
