#include "entity/media/TranscodeWorker.hpp"
#include "entity/media/HapTranscoder.hpp"

#include <iostream>
#include <utility>

namespace entity {

const char* TranscodeStateToString(TranscodeState s) {
    switch (s) {
        case TranscodeState::Queued:  return "Queued";
        case TranscodeState::Running: return "Running";
        case TranscodeState::Done:    return "Done";
        case TranscodeState::Failed:  return "Failed";
    }
    return "Unknown";
}

TranscodeWorker::TranscodeWorker(std::string srcPath,
                                 std::string dstPath,
                                 std::string variant,
                                 double srcFps)
    : m_srcPath(std::move(srcPath))
    , m_dstPath(std::move(dstPath))
    , m_variant(std::move(variant))
    , m_srcFps(srcFps)
{
    m_thread = std::thread([this]() { this->threadFunc(); });
}

TranscodeWorker::~TranscodeWorker() {
    requestCancel();
    if (m_thread.joinable()) m_thread.join();
}

TranscodeState TranscodeWorker::waitUntilFinished() {
    if (m_thread.joinable()) m_thread.join();
    return m_state.load(std::memory_order_acquire);
}

void TranscodeWorker::threadFunc() {
    m_state.store(TranscodeState::Running, std::memory_order_release);

    auto progressCb = [this](int64_t done, int64_t total) -> bool {
        m_framesDone.store(done, std::memory_order_relaxed);
        m_framesTotal.store(total, std::memory_order_relaxed);
        const float p = (total > 0)
            ? static_cast<float>(done) / static_cast<float>(total)
            : 0.0f;
        m_progress01.store(p, std::memory_order_relaxed);
        return !m_cancelRequested.load(std::memory_order_relaxed);
    };

    Result r = Result::Failure;
    try {
        r = transcodeToHap(m_srcPath, m_dstPath, m_variant, progressCb, m_srcFps);
    } catch (const std::exception& e) {
        std::cerr << "TranscodeWorker: exception: " << e.what() << std::endl;
        r = Result::Failure;
    } catch (...) {
        std::cerr << "TranscodeWorker: unknown exception" << std::endl;
        r = Result::Failure;
    }

    m_result.store(r, std::memory_order_release);
    const bool cancelled = m_cancelRequested.load(std::memory_order_relaxed);
    m_state.store(
        (r == Result::Success && !cancelled) ? TranscodeState::Done
                                             : TranscodeState::Failed,
        std::memory_order_release);
}

} // namespace entity
