// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "IMessageTransport.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <vector>

namespace entity::bus {

// Same-process implementation. Two FIFO queues guarded by per-direction
// mutexes; no worker threads. drain() moves the queue out under the lock
// then invokes the sink without holding it (so a sink that re-enters
// send() on the same direction can't deadlock).
//
// Latest-wins for RenderFrame on D2R:
//   A new RenderFrame sent on D2R supersedes any unconsumed prior one in
//   the channel — only the last one is ever delivered to the consumer.
//   All other message types keep strict FIFO ordering. This is safe because
//   RenderFrame is an immutable snapshot (bus CLAUDE.md §4); the consumer
//   never observed the dropped frames. Other D2R types (ProvisionClipResources,
//   SetOutputEnabled, etc.) are reply-bearing and must not be dropped.
//   R2D never carries RenderFrame; its channel is plain FIFO.
class InMemoryMessageTransport final : public IMessageTransport {
public:
    void send(Direction d, std::vector<std::uint8_t> bytes) override;
    void drain(Direction d, const Sink& sink) override;

    // Diagnostics for tests + telemetry. Returns total pending message count
    // (FIFO queue + 1 if a latest-only RenderFrame slot is occupied).
    std::size_t pending(Direction d) const;

private:
    struct Channel {
        mutable std::mutex mu;
        std::queue<std::vector<std::uint8_t>> q;
        // Latest-wins slots (D2R only; always empty on R2D).
        std::optional<std::vector<std::uint8_t>> latestRenderFrame;
        std::optional<std::vector<std::uint8_t>> latestSceneSnapshot;
    };

    Channel& channel(Direction d);
    const Channel& channel(Direction d) const;

    Channel m_d2r;
    Channel m_r2d;
};

} // namespace entity::bus
