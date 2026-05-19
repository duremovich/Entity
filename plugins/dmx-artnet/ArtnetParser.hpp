// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include <cstddef>
#include <cstdint>

namespace entity::dmx {

// Result of parsing an Art-Net packet. Only the ArtDmx opcode (0x5000)
// is recognized; everything else (ArtPoll, ArtPollReply, ArtSync,
// ArtIpProg, ...) is silently ignored.
struct ArtnetDmx {
    std::uint16_t       universe;       // 15-bit composite (Net:7 | SubUni:8)
    std::uint8_t        sequence;       // sender's sequence counter (unused)
    std::uint8_t        physical;       // physical input port (unused)
    std::uint16_t       channelCount;   // 1..512
    const std::uint8_t* channels;       // pointer into the input buffer
};

// Parses one UDP datagram. Returns true iff the packet validated as
// ArtDmx and `out` was populated. The `out.channels` pointer aliases
// the input `bytes` — the caller must not free `bytes` before
// finishing with the parsed result.
bool parseArtnetPacket(const std::uint8_t* bytes,
                       std::size_t         len,
                       ArtnetDmx&          out);

} // namespace entity::dmx
