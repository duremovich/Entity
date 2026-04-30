#pragma once

/**
 * ContentScanner — periodic poll of a project's `content/` folder for
 * media files added/removed by external tools (FreeFileSync, rsync,
 * robocopy, plain Explorer drag-drop).
 *
 * Design choices (#27):
 *   - Single persistent worker thread on a CV-based 2s wait_for. No
 *     per-task threading, no std::async churn.
 *   - Recursive walk of `<projectRoot>/content/`. Skips `.archive/`,
 *     `.cache/`, anything beginning with `.`, and common in-flight
 *     patterns (`*.tmp`, `*.ffs_tmp`, `~*`).
 *   - Whitelist of media extensions; non-media files in `content/` are
 *     ignored entirely (no point surfacing .json sidecars in MediaBin).
 *   - Three-signal stable-write gate: a discovered file must have
 *     unchanged `(size, mtime)` for two consecutive ticks AND open for
 *     read with no sharing AND live in a directory whose own mtime
 *     hasn't changed this tick.
 *   - Worker emits `ScanDelta`s into a mutex-protected queue. ALL
 *     library mutation happens on the main thread by draining the
 *     queue from `Engine::update`.
 *
 * The scanner does not know the ProjectManager's current library state.
 * The caller (Engine) is responsible for deduping discovered paths
 * against existing entries before calling `addMediaFile`. The library's
 * `addMediaFile` is itself idempotent so duplicates are no-ops, but
 * keeping that responsibility on Engine lets it also handle the
 * Linked-entry-pointing-into-content/ edge case.
 */

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace entity {

class ContentScanner {
public:
    enum class DeltaKind {
        Added,    // file appeared in content/ and passed the stable-write gate
        Removed,  // a previously-Added file is no longer on disk
    };

    struct ScanDelta {
        DeltaKind   kind;
        std::string relativePath;  // project-relative under content/, fwd-slashed
    };

    ContentScanner();
    ~ContentScanner();

    ContentScanner(const ContentScanner&)            = delete;
    ContentScanner& operator=(const ContentScanner&) = delete;

    /**
     * Start the worker thread polling `<projectRoot>/content/`. If the
     * scanner is already running, it is stopped first. Calling with an
     * empty path is equivalent to `stop()`.
     */
    void start(const std::filesystem::path& projectRoot);

    /** Stop the worker thread, joining it. Safe to call multiple times. */
    void stop();

    bool isRunning() const noexcept { return m_thread.joinable(); }

    /**
     * Drain all pending deltas. Returns them in emission order and
     * leaves the internal queue empty. Call from the main thread.
     */
    std::vector<ScanDelta> drain();

    /**
     * Run one scan synchronously on the *calling* thread. Used by tests
     * to drive the scanner deterministically without relying on the
     * 2-second timer. Production code should not call this.
     */
    void tickForTesting();

    /** Configure the polling interval (default 2s). For tests. */
    void setPollIntervalForTesting(std::chrono::milliseconds interval);

private:
    struct FileState {
        std::int64_t sizeBytes{0};
        std::int64_t mtimeUnix{0};
        int          stableTicks{0};  // increments while (size, mtime) unchanged; gate at 2
        bool         reported{false}; // true once Added has been emitted
    };

    void runLoop();
    void doScan();

    bool shouldSkipFile(const std::filesystem::path& filename) const;
    bool shouldSkipDir(const std::filesystem::path& dirname) const;
    bool isMediaExtension(const std::filesystem::path& ext) const;
    bool isOpenableForRead(const std::filesystem::path& absolute) const;

    void enqueue(ScanDelta d);

    std::filesystem::path m_projectRoot;
    std::filesystem::path m_contentDir;
    std::unordered_set<std::string> m_mediaExts;

    std::thread             m_thread;
    std::atomic<bool>       m_stop{false};
    std::condition_variable m_cv;
    std::mutex              m_cvMutex;
    std::chrono::milliseconds m_pollInterval{std::chrono::seconds(2)};

    // Worker-thread-only state.
    std::unordered_map<std::string, FileState> m_files;

    // Main-thread / worker shared output.
    std::mutex                m_queueMutex;
    std::vector<ScanDelta>    m_queue;
};

}  // namespace entity
