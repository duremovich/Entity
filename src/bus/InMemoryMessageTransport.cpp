// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich

#include "entity/bus/InMemoryMessageTransport.hpp"

#include <cstring>
#include <string_view>
#include <utility>

namespace entity::bus {

namespace {

// The wire format is byte-deterministic (nlohmann::ordered_json, no spaces,
// per bus CLAUDE.md). Every serialized message starts with this envelope;
// checking only the prefix is sufficient and avoids re-parsing the entire
// payload at send time.
constexpr std::string_view kRenderFramePrefix  = R"({"type":"RenderFrame")";
constexpr std::string_view kSceneSnapshotPrefix = R"({"type":"SceneSnapshot")";

bool isRenderFrameBytes(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() >= kRenderFramePrefix.size()
        && std::memcmp(bytes.data(),
                       kRenderFramePrefix.data(),
                       kRenderFramePrefix.size()) == 0;
}

bool isSceneSnapshotBytes(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() >= kSceneSnapshotPrefix.size()
        && std::memcmp(bytes.data(),
                       kSceneSnapshotPrefix.data(),
                       kSceneSnapshotPrefix.size()) == 0;
}

} // namespace

InMemoryMessageTransport::Channel& InMemoryMessageTransport::channel(Direction d) {
    return d == Direction::D2R ? m_d2r : m_r2d;
}

const InMemoryMessageTransport::Channel& InMemoryMessageTransport::channel(Direction d) const {
    return d == Direction::D2R ? m_d2r : m_r2d;
}

void InMemoryMessageTransport::send(Direction d, std::vector<std::uint8_t> bytes) {
    // Latest-wins for RenderFrame and SceneSnapshot on D2R: detect via byte-
    // prefix (envelope is deterministic per bus CLAUDE.md) and route into the
    // latest-only slot instead of the FIFO queue. The prior slot value (if
    // any) is silently dropped — it was never observed, and these messages
    // are immutable snapshots (bus CLAUDE.md §4).
    if (d == Direction::D2R) {
        if (isRenderFrameBytes(bytes)) {
            auto& c = m_d2r;
            std::lock_guard<std::mutex> lk(c.mu);
            c.latestRenderFrame = std::move(bytes);
            return;
        }
        if (isSceneSnapshotBytes(bytes)) {
            auto& c = m_d2r;
            std::lock_guard<std::mutex> lk(c.mu);
            c.latestSceneSnapshot = std::move(bytes);
            return;
        }
    }

    auto& c = channel(d);
    std::lock_guard<std::mutex> lk(c.mu);
    c.q.push(std::move(bytes));
}

void InMemoryMessageTransport::drain(Direction d, const Sink& sink) {
    auto& c = channel(d);

    // Steal both latest-wins slots and the FIFO queue under the lock,
    // then deliver after release. Sinks may re-enter send() on the same
    // direction (replies, fan-outs); holding the mutex across user code
    // would self-deadlock and stall unrelated producers.
    //
    // Delivery order: latest SceneSnapshot first (so show thread caches the
    // freshest scene state), then latest RenderFrame (uploads freshest clip
    // state), then FIFO queue (control messages in send order).
    std::optional<std::vector<std::uint8_t>> latestSS;
    std::optional<std::vector<std::uint8_t>> latestRF;
    std::queue<std::vector<std::uint8_t>> local;
    {
        std::lock_guard<std::mutex> lk(c.mu);
        latestSS = std::move(c.latestSceneSnapshot);
        c.latestSceneSnapshot.reset();
        latestRF = std::move(c.latestRenderFrame);
        c.latestRenderFrame.reset();
        std::swap(local, c.q);
    }

    if (latestSS) {
        sink(std::move(*latestSS));
    }
    if (latestRF) {
        sink(std::move(*latestRF));
    }
    while (!local.empty()) {
        sink(std::move(local.front()));
        local.pop();
    }
}

std::size_t InMemoryMessageTransport::pending(Direction d) const {
    const auto& c = channel(d);
    std::lock_guard<std::mutex> lk(c.mu);
    return c.q.size()
        + (c.latestRenderFrame.has_value()  ? 1u : 0u)
        + (c.latestSceneSnapshot.has_value() ? 1u : 0u);
}

} // namespace entity::bus
