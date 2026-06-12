#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
//
// Editor frame-ring index/fence bookkeeping, extracted as pure logic so the
// ring-key invariant (the slot the editor fences == the slot ImGui reuses) is
// unit-testable without a live D3D12 device. See
// docs/perf/device-hung-2026-06/diagnosis.md for the use-after-free this guards.
#include <cstdint>
#include <vector>

namespace entity::render {

class EditorFrameRing {
public:
    explicit EditorFrameRing(uint32_t numFramesInFlight)
        : m_n(numFramesInFlight),
          m_signaled(numFramesInFlight, 0),
          m_pending(numFramesInFlight, 1) {}

    // Call at frame start. swapChainIndex is accepted but NOT used as the ring
    // key — kept only so callers can document the legacy value alongside.
    void beginFrame(uint32_t /*swapChainIndex*/) {
        ++m_counter;                       // matches ImGui: ++ before use
        m_slot = m_counter % m_n;
        m_waitValue = m_pending[m_slot] > 0 ? m_pending[m_slot] - 1 : 0;
    }

    void endFrame() {
        m_signaled[m_slot] = m_pending[m_slot];
        ++m_pending[m_slot];               // next value to signal for this slot
    }

    uint32_t currentSlot() const { return m_slot; }
    uint64_t pendingWaitValue() const { return m_waitValue; }
    uint64_t lastSignaledForSlot(uint32_t slot) const { return m_signaled[slot]; }

private:
    uint32_t m_n;
    uint32_t m_counter = UINT32_MAX;       // free-running, ++ before first use -> 0
    uint32_t m_slot = 0;
    uint64_t m_waitValue = 0;
    std::vector<uint64_t> m_signaled;      // last value signaled per slot
    std::vector<uint64_t> m_pending;       // next value to signal per slot
};

} // namespace entity::render
