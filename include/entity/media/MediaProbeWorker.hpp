#pragma once

/**
 * MediaProbeWorker — off-thread FFmpeg metadata probe.
 *
 * Why this exists:
 *   Opening a media file with FFmpeg to read codec / resolution /
 *   framerate / duration costs 30-60 ms per file (avformat_open_input
 *   + find_stream_info + decoder alloc). Doing this on the render
 *   thread, lazily, on first row draw produced ~1s freezes when the
 *   MediaBin had a few dozen visible rows on startup. Worse, the
 *   freeze only happened once the bin's tab was active, so the bug
 *   read as "files don't load until I click the tab."
 *
 * Threading model:
 *   - Single persistent worker thread. Same pattern as ContentScanner /
 *     TranscodeWorker — no general thread pool.
 *   - Caller enqueues stored paths from the main thread. The path
 *     resolver lambda (Managed-relative -> absolute) is invoked on the
 *     caller's thread BEFORE the work item is queued, so the worker
 *     thread never touches ProjectManager state.
 *   - Result map is guarded by std::shared_mutex. tryGet (per-row, per
 *     frame, on the render thread) takes the shared lock; the worker
 *     takes the exclusive lock once per completed probe.
 */

#include "entity/media/MediaProbe.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace entity {

class MediaProbeWorker {
public:
    /**
     * @param resolveAbsolutePath  takes a stored library key (project-
     *        relative for Managed entries, absolute for Linked) and
     *        returns an absolute path FFmpeg can open. Invoked on the
     *        thread that calls enqueue() — not on the worker — so the
     *        callback can safely touch ProjectManager state.
     */
    explicit MediaProbeWorker(
        std::function<std::string(const std::string&)> resolveAbsolutePath);
    ~MediaProbeWorker();

    MediaProbeWorker(const MediaProbeWorker&)            = delete;
    MediaProbeWorker& operator=(const MediaProbeWorker&) = delete;

    /**
     * Idempotent. No-op if the path has already been seen (queued,
     * in-flight, or completed). Resolution to absolute happens on the
     * calling thread.
     */
    void enqueue(const std::string& storedPath);

    /**
     * Non-blocking lookup. Empty optional means the probe hasn't
     * completed yet — render a placeholder.
     */
    std::optional<ProbeInfo> tryGet(const std::string& storedPath) const;

    // Counters for status UI. Atomic — transient inconsistencies between
    // pending/done/total across one frame are acceptable.
    std::size_t pendingCount() const noexcept;
    std::size_t doneCount() const noexcept;
    std::size_t totalEnqueuedSinceReset() const noexcept;

    /**
     * Drop the per-load counters so a fresh project's progress UI
     * starts at 0/0. Does NOT clear the result cache — past probes stay
     * valid (cached entries returning instantly is the whole point).
     */
    void resetCounters();

private:
    struct QueueEntry {
        std::string storedPath;
        std::string absolutePath;
    };

    void      runLoop();
    ProbeInfo doProbe(const std::string& absolutePath) const;

    std::function<std::string(const std::string&)> m_resolveAbsolutePath;

    // Queue + dedupe set. m_known holds every storedPath we've ever
    // accepted in this worker's lifetime (queued / in-flight / done) so
    // enqueue() is O(1) and idempotent without scanning the deque or
    // taking the results lock.
    mutable std::mutex              m_queueMutex;
    std::deque<QueueEntry>          m_queue;
    std::unordered_set<std::string> m_known;
    std::condition_variable         m_cv;

    // Result map. shared_mutex so per-row tryGet on the render thread
    // shares the lock; worker takes exclusive only on completion.
    mutable std::shared_mutex                  m_resultsMutex;
    std::unordered_map<std::string, ProbeInfo> m_results;

    std::atomic<std::size_t> m_pending{0};         // queued + in-flight
    std::atomic<std::size_t> m_doneSinceReset{0};
    std::atomic<std::size_t> m_totalSinceReset{0};

    std::atomic<bool> m_stop{false};
    std::thread       m_thread;
};

}  // namespace entity
