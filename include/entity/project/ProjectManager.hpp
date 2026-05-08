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
#include "entity/media/MediaProbe.hpp"
#include <cstdint>
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
     * If `errorOut` is non-null, a UI-friendly description of the failure
     * is written to it. Permission-denied errors are detected and produce
     * an admin-aware hint (Issue #22).
     *
     * Note: callable before Engine attaches a Timeline. The empty
     * project is written by routing through ProjectSerializer with a
     * temporary Timeline, so the schema version automatically tracks
     * `ProjectSerializer::PROJECT_VERSION` rather than hand-rolled JSON.
     */
    bool createNew(const std::filesystem::path& parentDir,
                   const std::string& projectName,
                   std::string* errorOut = nullptr);

    /**
     * Save the current timeline to `filepath`. Empty path uses the currently
     * set project path, or "project.entity" if nothing has been saved yet.
     * Updates the current project path on success.
     */
    bool save(const std::filesystem::path& filepath = "");

    /**
     * Save-As-Bundle (ADR-0009). Creates a fully-structured project folder
     * at `<parentDir>/<projectName>/`, recursively copies the current
     * project's content/ + presets/ + objects/ + exports/ + snapshots/
     * trees alongside, then writes the .entity file at the new root.
     * `.cache/` is deliberately excluded — it's machine-local and
     * regenerable, not part of the portable bundle.
     *
     * On success, `m_projectPath` is updated to the new file (the
     * caller is now working in the bundle copy) and Managed paths
     * resolve against the new root.
     *
     * Refuses (returns false) if:
     *   - `projectName` is empty or contains a path separator
     *   - `<parentDir>/<projectName>` already exists on disk
     *   - the current project hasn't been saved at least once
     *     (no source content tree to copy from)
     *   - the destination filesystem rejects directory creation /
     *     copy / write
     *
     * Linked entries are unaffected — their absolute paths still point
     * at the user's existing media. Bundle portability only applies to
     * Managed entries.
     */
    bool saveAsBundle(const std::filesystem::path& parentDir,
                      const std::string& projectName);

    /**
     * Load a project file, clearing and re-populating clip decode state via
     * the renderer. Updates the current project path on success.
     */
    bool load(const std::filesystem::path& filepath);

    /**
     * ADR-0009 — drop all project-scoped state owned by ProjectManager:
     * the project path, the media library, and the non-HAP-import policy
     * (which is persisted with the project). Used by `Engine::closeProject`
     * when returning to the launcher; the editor entity-side teardown is
     * Engine's responsibility.
     */
    void closeProject();

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
        // archive exists. Persisted only when no archive exists.
        std::string originalCodec;

        // Cached MediaProbeWorker output. Persisted (v12+) so reopening
        // a project doesn't re-probe every file from scratch. Validity
        // is gated on size + mtime matching the on-disk file: if they
        // match, the worker is seeded directly from `cachedProbe` and
        // no FFmpeg open happens; otherwise the worker re-probes and
        // the cache is updated. `lastProbeSizeBytes == 0` means
        // "never probed" (fresh entry, or a pre-v12 file).
        std::int64_t lastProbeSizeBytes{0};
        std::int64_t lastProbeMtimeUnix{0};
        ProbeInfo    cachedProbe{};

        // Transient — set by ContentScanner when the file's no longer on
        // disk (#27). NOT serialized; reset to false on every load. UI
        // greys missing rows; the entry stays in the library so brief
        // unlink-then-rename windows during external sync don't nuke
        // user state.
        bool missingOnDisk{false};
    };

    /**
     * Register `originalPath`. Idempotent — if an entry exists it is not
     * touched. Returns a reference to the (possibly-existing) entry.
     *
     * `kind` defaults to `Linked` to match the pre-v7 behavior; the
     * structured-import flow passes `Managed` when copying the file
     * into `content/`. The kind is set only on first registration —
     * re-calling `addMediaFile` for an existing entry leaves the
     * kind unchanged.
     */
    MediaLibraryEntry& addMediaFile(const std::string& originalPath,
                                    PathKind kind = PathKind::Linked);

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
     * Find the latest version of any media in the same group as
     * `storedPath`, scoped to the same `pathKind` (Linked and Managed
     * never cross-group). Returns nullptr if the group has no members.
     *
     * The "latest" is the entry whose version tag sorts highest under
     * `compareVersionTags` (smart numeric-or-lexicographic). Empty tags
     * sort first (i.e. an unversioned `intro.mov` is the earliest member
     * of the `intro` group).
     *
     * Issue #27 (filename-based versioning).
     */
    const MediaLibraryEntry* findLatestInGroup(const std::string& storedPath) const;

    /**
     * Remove the entry. Caller is responsible for any in-flight transcode
     * worker.
     */
    void removeMediaFile(const std::string& originalPath);

    /**
     * Resolve a stored media path to an absolute filesystem path the
     * decoder / transcoder / FFmpeg can open. Routes by `pathKind`:
     *
     *   - `Managed` entries store project-relative paths
     *     (e.g. `content/act1/intro.mov`); the helper joins them with
     *     the current project root.
     *   - `Linked` entries store absolute paths and are returned
     *     unchanged.
     *
     * Lookup is via `findEntry`, so `storedPath` must match the same
     * string used to register the entry. A path that isn't in the
     * library (or empty) is returned as-is — useful for code paths
     * that haven't gone through `addMediaFile` yet (e.g. fresh
     * imports during ingestion).
     *
     * If no project root is set yet, Managed paths fall back to
     * being returned as-is. The caller is responsible for treating
     * that as "can't open until project loaded."
     */
    std::string resolveMediaPath(const std::string& storedPath) const;

    /**
     * Resolve which file path the decoder should actually open for a clip
     * whose stored `filepath` is `originalPath`. Returns the transcoded
     * path when available and on-disk, else the original — both routed
     * through `resolveMediaPath` so Managed entries resolve against the
     * project root.
     */
    std::string decoderPathFor(const std::string& originalPath) const;

    // --- Orphan-showfile recovery (ADR-0009) ----------------------------------

    /**
     * Idempotent mkdir of every canonical subdirectory `createNew()`
     * would have produced: `content/unsorted/`, `presets/`, `objects/`,
     * `exports/`, `snapshots/`, `.cache/thumbnails/`. Useful when the
     * user opens a "naked" .entity file (e.g. saved with
     * `Save Project As...` rather than `Save Project As Bundle...`)
     * and wants to bring the tree back to a usable shape.
     *
     * Returns the number of directories that were actually created
     * (zero on a fully-formed project).
     */
    int rebuildStructure();

    /**
     * Outcome of one `findMissingManagedMedia` pass. The caller (Engine)
     * uses this to surface a summary log line and to decide whether to
     * reload the project so freshly-restored files get attached to clips.
     */
    struct FindMediaResult {
        int missingBefore{0};   // Count of Managed entries with missing files at start.
        int matched{0};         // Filenames found in the search dir.
        int copied{0};          // Successfully copied into the canonical path.
        int stillMissing{0};    // missingBefore - copied (no match or copy failed).
    };

    /**
     * Roll a Managed entry back to its pre-transcode state. Only valid
     * when the entry has an `archivedOriginal` file on disk:
     *   1. Delete the HAP file at the canonical content path.
     *   2. Rename `<root>/<archivedOriginal>` -> the canonical content path.
     *   3. Clear `archivedOriginal`, `originalCodec`, `transcodedPath`,
     *      and `variant` on the entry.
     *
     * After a successful restore the entry looks the same as a freshly
     * Copy-imported never-transcoded clip. If the user wants HAP back
     * they re-trigger transcode (which will archive again).
     *
     * Returns false (state unchanged) if:
     *   - entry not in library / not Managed / no archive set
     *   - archive file or canonical path can't be found / written
     *
     * Caller (Engine) is responsible for removing any in-flight
     * TranscodeManager worker for this canonical path and for
     * reloading the project so the decoder picks up the new codec.
     */
    bool restoreOriginal(const std::string& canonicalPath);

    /**
     * Recursively walks `searchDir`, builds an index of media filenames
     * found there, then for each Managed mediaLibrary entry whose
     * canonical path doesn't currently exist on disk, looks up the
     * entry's filename in the index. Matches are copied into the
     * canonical content path (parent dirs created as needed). The
     * source files in `searchDir` are NOT moved or deleted.
     *
     * Multiple files in `searchDir` with the same name resolve to the
     * first one encountered during the walk. Linked entries are
     * untouched; the operation is scoped to Managed entries because
     * those have a deterministic canonical path the caller can
     * restore. Linked files are by design at user-controlled paths.
     */
    FindMediaResult findMissingManagedMedia(const std::filesystem::path& searchDir);

    // --- Collect Linked → Managed (ADR-0009) ---------------------------------

    /**
     * Outcome of a single entry's collect step. `oldOriginalPath` matched
     * what the entry had on entry to the operation; `newOriginalPath` is
     * the new project-relative `originalPath` after the flip. Engine's
     * registry walk uses this map to rewrite `clip.filepath`.
     */
    struct CollectResult {
        // Per-entry mapping. Keys = old absolute originalPath, values =
        // new project-relative originalPath. Engine walks the clip
        // registry and rewrites `clip.filepath` using this map.
        std::vector<std::pair<std::string, std::string>> rewrites;
        int  collected{0};       // Entries successfully flipped to Managed.
        int  missing{0};         // Linked entries whose source no longer resolves.
        int  alreadyManaged{0};  // Skipped (no work needed).
    };

    /**
     * One-shot v6 → v7 conversion: walks the media library, copies every
     * `Linked` entry whose source resolves into
     * `<projectRoot>/content/<subfolder>/<filename>` (collision-suffixed),
     * archives the original under `.archive/<filename>` if a cache-dir
     * transcode exists (and moves the transcode to the canonical content
     * path), then flips the entry to `Managed`. Linked entries whose
     * source doesn't resolve are left alone (logged via `result.missing`)
     * so the operator can decide whether to relink or remove.
     *
     * Original source files at their pre-collect paths are NOT deleted —
     * the operation is non-destructive on the user's disk outside the
     * project folder.
     *
     * Caller is responsible for rewriting any external references to the
     * old `originalPath` (e.g. `clip.filepath` on Engine's registry) using
     * `result.rewrites`.
     *
     * Returns an empty result with `collected == 0` if no project is
     * loaded or the project root isn't writable.
     */
    CollectResult collectLinkedIntoProject(const std::string& subfolder = "unsorted");

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
