#pragma once

/**
 * ContentScanner — periodic poll of a project's `content/` and
 * `objects/` folders for files added/removed by external tools
 * (FreeFileSync, rsync, robocopy, plain Explorer drag-drop).
 *
 * Design choices (#27):
 *   - Single persistent worker thread on a CV-based 2s wait_for. No
 *     per-task threading, no std::async churn.
 *   - Walks two roots under the project: `content/` (media files —
 *     .mov, .mp4, .png, …) and `objects/` (3D models — currently
 *     `.obj` only). Each walk uses its own extension whitelist; deltas
 *     carry a `DeltaSource` so Engine can dispatch to the right library.
 *   - Skips `.archive/`, `.cache/`, anything beginning with `.`, and
 *     common in-flight patterns (`*.tmp`, `*.ffs_tmp`, `~*`).
 *   - Whitelist of relevant extensions per root; non-whitelisted files
 *     are ignored entirely (no point surfacing .json sidecars in the
 *     MediaBin or .mtl files in the ModelBin).
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
 * against existing entries before calling `addMediaFile` / `addObjectFile`.
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
        Added,    // file appeared and passed the stable-write gate
        Removed,  // a previously-Added file is no longer on disk
    };

    /**
     * Which library the delta is for. Drives Engine's dispatch between
     * the media library (ProjectManager::addMediaFile + MediaProbeWorker)
     * and the object library (ProjectManager::addObjectFile + ObjLoader).
     */
    enum class DeltaSource {
        Media,
        Object,
        // <project>/effects — user-authored HLSL effect packs (.hlsl /
        // .json manifests / .hlsli). Unlike Media/Object, a CHANGED
        // effect file re-emits Added once it re-stabilizes: the whole
        // point of watching this root is hot reload, whereas in-place
        // media rewrites stay silent by design (transcode replace).
        EffectSource,
    };

    struct ScanDelta {
        DeltaKind   kind;
        DeltaSource source;
        std::string relativePath;  // project-relative, fwd-slashed
    };

    ContentScanner();
    ~ContentScanner();

    ContentScanner(const ContentScanner&)            = delete;
    ContentScanner& operator=(const ContentScanner&) = delete;

    /**
     * Start the worker thread polling `<projectRoot>/content/` (media)
     * and `<projectRoot>/objects/` (3D models). If the scanner is
     * already running, it is stopped first. Calling with an empty path
     * is equivalent to `stop()`.
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

    /**
     * Free-standing filters that match the polling scanner's rules.
     * Engine's initial-sync scan calls these so the in-process
     * "have we seen this file already" decision uses the same whitelist
     * as the background poller — files dropped before the app started
     * shouldn't show up under different rules than ones dropped after.
     */
    static bool isMediaExtensionStatic(const std::filesystem::path& ext);
    static bool isObjectExtensionStatic(const std::filesystem::path& ext);
    static bool shouldSkipFileStatic(const std::filesystem::path& filename);
    static bool shouldSkipDirStatic(const std::filesystem::path& dirname);

private:
    struct FileState {
        DeltaSource  source{DeltaSource::Media};
        std::int64_t sizeBytes{0};
        std::int64_t mtimeUnix{0};
        int          stableTicks{0};  // increments while (size, mtime) unchanged; gate at 2
        bool         reported{false}; // true once Added has been emitted
    };

    struct WatchRoot {
        DeltaSource           source;
        std::filesystem::path absoluteDir;
        const std::unordered_set<std::string>* extensions;  // non-owning
    };

    void runLoop();
    void doScan();
    void scanRoot(const WatchRoot& root,
                  std::unordered_map<std::string, FileState>& seen);

    bool shouldSkipFile(const std::filesystem::path& filename) const;
    bool shouldSkipDir(const std::filesystem::path& dirname) const;
    bool isOpenableForRead(const std::filesystem::path& absolute) const;

    void enqueue(ScanDelta d);

    std::filesystem::path  m_projectRoot;
    std::vector<WatchRoot> m_roots;

    std::thread             m_thread;
    std::atomic<bool>       m_stop{false};
    std::condition_variable m_cv;
    std::mutex              m_cvMutex;
    std::chrono::milliseconds m_pollInterval{std::chrono::seconds(2)};

    // Worker-thread-only state. Keyed by project-relative path (which
    // is unique across roots because the roots are sibling directories).
    std::unordered_map<std::string, FileState> m_files;

    // Main-thread / worker shared output.
    std::mutex                m_queueMutex;
    std::vector<ScanDelta>    m_queue;
};

}  // namespace entity
