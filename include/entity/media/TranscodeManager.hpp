#pragma once

/**
 * TranscodeManager — owns the set of TranscodeWorkers driving background
 * HAP re-encodes for freshly-imported media. Keyed by the absolute source
 * path. One worker per source; re-enqueuing the same path while a worker
 * already exists is a no-op (returns the existing destination).
 *
 * Cache layout:
 *   <cacheDir>/<src-stem>_<hash>.<variant>.mov
 *
 * The short hash (8 hex chars of FNV-1a over the absolute source path)
 * keeps the filename stable across sessions and avoids collisions between
 * same-stem files in different directories.
 *
 * Thread-safety: all public methods lock an internal mutex. Query methods
 * return plain values (state, progress snapshots) — no borrowed references
 * to worker internals, so callers can't race with the worker thread.
 */

#include "entity/core/Types.hpp"
#include "entity/media/TranscodeWorker.hpp"

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace entity {

struct TranscodeStatus {
    TranscodeState state{TranscodeState::Queued};
    float          progress01{0.0f};
    int64_t        framesDone{0};
    int64_t        framesTotal{0};
    Result         result{Result::Success};
    std::string    outputPath;   // Only valid when state == Done
    std::string    variant;
};

class TranscodeManager {
public:
    TranscodeManager() = default;
    ~TranscodeManager() = default;

    TranscodeManager(const TranscodeManager&) = delete;
    TranscodeManager& operator=(const TranscodeManager&) = delete;

    /**
     * Set the directory where transcoded outputs are written. Created on
     * first enqueue if it doesn't exist. Defaults to
     *   <temp>/entity_hap_cache
     * so something always works even without a saved project.
     */
    void setCacheDir(const std::filesystem::path& dir);
    const std::filesystem::path& cacheDir() const { return m_cacheDir; }

    /**
     * Enqueue a transcode. Returns the destination path the worker is
     * (or was) writing to, regardless of current state — callers can
     * store this with the media library entry right away.
     *
     * If a worker for `absSrcPath` already exists in any state, returns
     * its destination without starting a new worker (idempotent).
     *
     * @param srcFps > 0 for image-sequence sources (e.g. "x/frame_%03d.png");
     *               0 for regular containers.
     */
    std::string enqueue(const std::string& absSrcPath,
                        const std::string& variant = "hap_alpha",
                        double srcFps = 0.0);

    /// Returns true if a worker exists for this source (any state).
    bool has(const std::string& absSrcPath) const;

    /// Snapshot of the worker's state, or nullopt if no worker exists.
    std::optional<TranscodeStatus> statusOf(const std::string& absSrcPath) const;

    /**
     * Request cancel on the worker if it exists. Worker exits at its next
     * frame boundary; the (partial) output file is left on disk so the
     * caller can delete it if desired.
     */
    void cancel(const std::string& absSrcPath);

    /**
     * Remove workers whose state is Done. Failed workers are LEFT IN PLACE
     * so the MediaBin can keep showing the failure + offer a Retry menu
     * item; without this, a fast-failing transcode (e.g. 4-alignment
     * rejection by the HAP encoder) vanished from the UI within one
     * frame. Use remove(src) or joinAll() to drop a Failed worker.
     */
    void clearDone();

    /**
     * Drop a specific worker by source path. Requests cancel, joins the
     * thread, erases the map entry. Safe in any state. Used by the
     * MediaBin "Retry transcode" flow to wipe the Failed worker before
     * enqueuing a fresh one.
     */
    void remove(const std::string& absSrcPath);

    /// All workers, joined. Call from Engine shutdown.
    void joinAll();

    // --- Exposed for tests / debug -----------------------------------------

    /**
     * Derive the destination path the manager would use for (absSrcPath,
     * variant) without actually starting a worker. Useful when loading a
     * project so the media library can check whether a cached transcode
     * already exists on disk before kicking off work.
     */
    std::filesystem::path deriveDstPath(const std::string& absSrcPath,
                                         const std::string& variant) const;

private:
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::unique_ptr<TranscodeWorker>> m_workers;
    std::filesystem::path m_cacheDir;
};

// --- Free helpers (tested directly) ---------------------------------------

/**
 * True if `mediaType` refers to a HAP variant. Used at import time to skip
 * enqueuing a no-op transcode.
 */
bool isHapMediaType(MediaType mediaType);

/**
 * 8-char lowercase hex FNV-1a over an absolute path. Deterministic across
 * sessions/OSes, stable vs. filename collisions.
 */
std::string shortHashOfPath(const std::string& absPath);

} // namespace entity
