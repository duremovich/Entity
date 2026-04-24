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

    /**
     * Attach the dependencies ProjectManager needs. Called by Engine after
     * it has constructed all three. Safe to call once; don't re-initialize.
     */
    void initialize(Timeline* timeline, entt::registry* registry, IRenderer* renderer);

    // --- Save / load ---------------------------------------------------------

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
     * One entry per imported source file. `originalPath` is the user-picked
     * path (shown in the bin, used as the canonical identity of the clip's
     * source). `transcodedPath` is the HAP variant the TranscodeManager
     * produced, empty until the worker finishes (or forever if auto-
     * transcode was off). `variant` records which HAP variant was written
     * so a re-transcode with a different variant doesn't silently overwrite.
     *
     * When the decoder opens a clip, it calls `decoderPathFor(clip.filepath)`
     * to pick between original and transcoded.
     */
    struct MediaLibraryEntry {
        std::string originalPath;
        std::string transcodedPath;   // empty if not transcoded (yet)
        std::string variant;          // e.g. "hap_alpha"; empty if transcoded empty
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
     * When true, non-HAP files dropped into the bin are queued for
     * background transcode. Persisted with the project. User-toggleable
     * via the MediaBin toolbar.
     */
    bool autoTranscodeOnImport() const         { return m_autoTranscodeOnImport; }
    void setAutoTranscodeOnImport(bool enable) { m_autoTranscodeOnImport = enable; }

private:
    // Non-owning dependencies (Engine owns and outlives this)
    Timeline*        m_timeline{nullptr};
    entt::registry*  m_registry{nullptr};
    IRenderer*       m_renderer{nullptr};

    std::filesystem::path           m_projectPath;
    std::vector<MediaLibraryEntry>  m_loadedMediaFiles;
    bool                            m_autoTranscodeOnImport{true};

    double m_autosaveInterval{30.0};
    double m_autosaveAccumulator{0.0};
};

} // namespace entity
