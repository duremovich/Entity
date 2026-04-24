#pragma once

/**
 * TranscodeWorker — one std::thread running `transcodeToHap` with atomic
 * progress + state for UI polling.
 *
 * Owned by TranscodeManager; callers should go through the manager rather
 * than constructing workers directly. Destructor requests cancel + joins,
 * so losing the owning manager shuts workers down cleanly.
 *
 * Thread-safety: all query methods (state(), progress01(), framesDone/Total(),
 * result(), outputPath()) are safe to call from any thread while the worker
 * is running. requestCancel() is also thread-safe; the worker checks the
 * flag from inside the transcode progress callback (~once per encoded frame).
 */

#include "entity/core/Types.hpp"
#include <atomic>
#include <string>
#include <thread>

namespace entity {

enum class TranscodeState : uint8_t {
    Queued,   // Worker constructed, thread not yet started
    Running,  // Encoding in progress
    Done,     // Finished, output file is valid HAP
    Failed,   // Encoder returned non-Success, or cancel requested
};

const char* TranscodeStateToString(TranscodeState s);

class TranscodeWorker {
public:
    /**
     * Construct + start a transcode. Kicks off the background thread
     * immediately; no separate start() call.
     *
     * @param srcPath    absolute source file (or "dir/frame_%03d.png" for
     *                   image sequences — `srcFps` must be > 0 then)
     * @param dstPath    absolute destination (.mov). Parent dir must exist.
     * @param variant    HAP variant: "hap", "hap_alpha" (default), "hap_q"
     * @param srcFps     > 0 for image sequences; 0 for video containers
     */
    TranscodeWorker(std::string srcPath,
                    std::string dstPath,
                    std::string variant = "hap_alpha",
                    double srcFps = 0.0);

    ~TranscodeWorker();

    TranscodeWorker(const TranscodeWorker&) = delete;
    TranscodeWorker& operator=(const TranscodeWorker&) = delete;

    // Query (thread-safe)
    TranscodeState state() const          { return m_state.load(std::memory_order_acquire); }
    float          progress01() const     { return m_progress01.load(std::memory_order_relaxed); }
    int64_t        framesDone() const     { return m_framesDone.load(std::memory_order_relaxed); }
    int64_t        framesTotal() const    { return m_framesTotal.load(std::memory_order_relaxed); }
    Result         result() const         { return m_result.load(std::memory_order_acquire); }
    const std::string& srcPath() const    { return m_srcPath; }
    const std::string& dstPath() const    { return m_dstPath; }
    const std::string& variant() const    { return m_variant; }

    // Control (thread-safe). Cancel is best-effort: the worker will exit at
    // its next frame boundary, the partial output file is left on disk
    // (caller is responsible for cleanup) and state flips to Failed.
    void requestCancel() { m_cancelRequested.store(true, std::memory_order_release); }
    bool cancelRequested() const { return m_cancelRequested.load(std::memory_order_relaxed); }

    // Blocking wait — primarily for tests. Returns terminal state.
    TranscodeState waitUntilFinished();

private:
    void threadFunc();

    std::string m_srcPath;
    std::string m_dstPath;
    std::string m_variant;
    double      m_srcFps{0.0};

    std::atomic<TranscodeState> m_state{TranscodeState::Queued};
    std::atomic<float>          m_progress01{0.0f};
    std::atomic<int64_t>        m_framesDone{0};
    std::atomic<int64_t>        m_framesTotal{0};
    std::atomic<Result>         m_result{Result::Success};
    std::atomic<bool>           m_cancelRequested{false};

    std::thread m_thread;
};

} // namespace entity
