// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "IMessageTransport.hpp"

#include <cstddef>
#include <mutex>
#include <queue>

namespace entity::bus {

// Same-process implementation. Two FIFO queues guarded by per-direction
// mutexes; no worker threads. drain() moves the queue out under the lock
// then invokes the sink without holding it (so a sink that re-enters
// send() on the same direction can't deadlock).
class InMemoryMessageTransport final : public IMessageTransport {
public:
    void send(Direction d, std::vector<std::uint8_t> bytes) override;
    void drain(Direction d, const Sink& sink) override;

    // Diagnostics for tests + telemetry. Snapshot count; can change between
    // call and use under contention.
    std::size_t pending(Direction d) const;

private:
    struct Channel {
        mutable std::mutex mu;
        std::queue<std::vector<std::uint8_t>> q;
    };

    Channel& channel(Direction d);
    const Channel& channel(Direction d) const;

    Channel m_d2r;
    Channel m_r2d;
};

} // namespace entity::bus
