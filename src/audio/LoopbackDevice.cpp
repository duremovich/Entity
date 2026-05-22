#include "entity/audio/LoopbackDevice.hpp"
#include "entity/profile/Tracy.hpp"
#include <chrono>

namespace entity {

bool LoopbackDevice::start(int requestedSampleRate, int channels,
                           FillCallback fill) {
    if (m_running.load()) return true;
    m_sampleRate = requestedSampleRate;
    m_channels   = channels;
    m_fill       = std::move(fill);
    m_framesPlayed.store(0);
    m_running.store(true);
    m_thread = std::thread(&LoopbackDevice::run, this);
    return true;
}

void LoopbackDevice::stop() {
    if (!m_running.exchange(false)) return;
    if (m_thread.joinable()) m_thread.join();
}

void LoopbackDevice::run() {
    tracy::SetThreadName("AudioLoopback");
    constexpr uint32_t kBlock = 480;                 // 10 ms @ 48 kHz
    std::vector<float> block(kBlock * m_channels);
    const auto period = std::chrono::duration<double>(
        double(kBlock) / m_sampleRate);
    auto next = std::chrono::steady_clock::now();
    while (m_running.load()) {
        if (m_fill) m_fill(block.data(), kBlock);
        {
            std::lock_guard<std::mutex> lk(m_captureMutex);
            m_capture.insert(m_capture.end(), block.begin(), block.end());
        }
        m_framesPlayed.fetch_add(kBlock);
        next += std::chrono::duration_cast<
            std::chrono::steady_clock::duration>(period);
        std::this_thread::sleep_until(next);
    }
}

std::vector<float> LoopbackDevice::capturedCopy() const {
    std::lock_guard<std::mutex> lk(m_captureMutex);
    return m_capture;
}

void LoopbackDevice::clearCapture() {
    std::lock_guard<std::mutex> lk(m_captureMutex);
    m_capture.clear();
}

} // namespace entity
