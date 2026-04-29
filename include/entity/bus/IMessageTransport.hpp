// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "Message.hpp"

#include <cstdint>
#include <functional>
#include <vector>

namespace entity::bus {

// Two-channel byte transport between Director and Renderer.
//
// Bytes-only by design: the Serialization layer turns Message <-> bytes
// independently of the transport. That keeps the Phase E swap (in-memory ->
// UDP, in a single transport implementation) decoupled from message-format
// changes.
//
// Threading contract:
//   - send() is callable from any thread, any time.
//   - drain() is called on the consumer side at the start of its tick.
//     The provided sink runs on the calling thread; the implementation may
//     invoke it zero or more times in FIFO order.
//   - send() during drain() is allowed (a sink that bounces a reply back).
class IMessageTransport {
public:
    using Sink = std::function<void(std::vector<std::uint8_t>&&)>;

    virtual ~IMessageTransport() = default;

    virtual void send(Direction d, std::vector<std::uint8_t> bytes) = 0;
    virtual void drain(Direction d, const Sink& sink) = 0;
};

} // namespace entity::bus
