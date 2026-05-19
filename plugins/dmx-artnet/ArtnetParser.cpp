// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "ArtnetParser.hpp"

#include <cstring>

namespace entity::dmx {

namespace {
constexpr std::uint16_t kOpArtDmx       = 0x5000;
constexpr std::size_t   kHeaderSize     = 18; // up through channelCount LSB
constexpr const char    kArtnetMagic[8] = {'A','r','t','-','N','e','t','\0'};
} // namespace

bool parseArtnetPacket(const std::uint8_t* bytes,
                       std::size_t         len,
                       ArtnetDmx&          out) {
    if (bytes == nullptr || len < kHeaderSize) return false;
    if (std::memcmp(bytes, kArtnetMagic, 8) != 0) return false;

    // Opcode is little-endian per the Art-Net spec.
    const std::uint16_t opcode =
        static_cast<std::uint16_t>(bytes[8]) |
        (static_cast<std::uint16_t>(bytes[9]) << 8);
    if (opcode != kOpArtDmx) return false;

    // Protocol version (big-endian; current value 14). Tolerate >= 14 so
    // future minor bumps don't break us; reject older.
    const std::uint16_t protocolVersion =
        (static_cast<std::uint16_t>(bytes[10]) << 8) |
        static_cast<std::uint16_t>(bytes[11]);
    if (protocolVersion < 14) return false;

    const std::uint8_t  sequence = bytes[12];
    const std::uint8_t  physical = bytes[13];
    // Universe: bytes[14] = SubUni (low byte), bytes[15] = Net (high byte,
    // 7 bits). 15-bit composite.
    const std::uint16_t universe =
        static_cast<std::uint16_t>(bytes[14]) |
        (static_cast<std::uint16_t>(bytes[15] & 0x7F) << 8);

    // Length is big-endian.
    const std::uint16_t length =
        (static_cast<std::uint16_t>(bytes[16]) << 8) |
        static_cast<std::uint16_t>(bytes[17]);
    if (length == 0 || length > 512) return false;
    if (kHeaderSize + length > len) return false;

    out.universe     = universe;
    out.sequence     = sequence;
    out.physical     = physical;
    out.channelCount = length;
    out.channels     = bytes + kHeaderSize;
    return true;
}

} // namespace entity::dmx
