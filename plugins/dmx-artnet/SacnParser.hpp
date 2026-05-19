// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include <cstddef>
#include <cstdint>

namespace entity::dmx {

// One decoded sACN (E1.31) DMX-data packet.
struct SacnDmx {
    std::uint16_t       universe;       // 1..63999 per spec, but we don't enforce
    std::uint8_t        priority;       // 0..200; 100 = default
    std::uint8_t        sequence;       // sender's sequence counter (unused)
    std::uint16_t       channelCount;   // 1..512 (excluding start code)
    const std::uint8_t* channels;       // pointer into the input buffer
};

// Parses one UDP datagram as sACN. Returns true iff every nested PDU
// validated and the payload is a DMX-data packet (DMP vector 0x02).
// Synchronization packets (universe 0, framing vector 0x00000003) are
// silently rejected — they're rare and out-of-scope for v1.
bool parseSacnPacket(const std::uint8_t* bytes,
                     std::size_t         len,
                     SacnDmx&            out);

} // namespace entity::dmx
