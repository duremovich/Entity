// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "SacnParser.hpp"

#include <cstring>

namespace entity::dmx {

namespace {

constexpr std::size_t kRootStart    = 0;
constexpr std::size_t kFramingStart = 38;
constexpr std::size_t kDmpStart     = 115;
constexpr std::size_t kDmpHeaderEnd = 125; // first channel byte = bytes[126]
constexpr std::size_t kPropertyValuesStart = 126;

constexpr const char kAcnPacketId[12] = {
    'A','S','C','-','E','1','.','1','7','\0','\0','\0'
};

constexpr std::uint32_t kRootVectorE131 = 0x00000004;
constexpr std::uint32_t kFramingVectorData = 0x00000002;
constexpr std::uint8_t  kDmpVector = 0x02;

std::uint16_t readU16BE(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}

std::uint32_t readU32BE(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8)  |
            static_cast<std::uint32_t>(p[3]);
}

} // namespace

bool parseSacnPacket(const std::uint8_t* bytes,
                     std::size_t         len,
                     SacnDmx&            out) {
    if (bytes == nullptr || len < kPropertyValuesStart + 1) return false;

    // --- Root Layer PDU ---
    // bytes[0..1]   preamble size = 0x0010
    // bytes[2..3]   postamble size = 0x0000
    // bytes[4..15]  ACN packet identifier "ASC-E1.17\0\0\0"
    // bytes[16..17] flags+length (don't validate length precisely)
    // bytes[18..21] vector = 0x00000004 (E1.31 data)
    // bytes[22..37] CID (UUID, ignored)
    if (readU16BE(bytes + 0) != 0x0010) return false;
    if (readU16BE(bytes + 2) != 0x0000) return false;
    if (std::memcmp(bytes + 4, kAcnPacketId, 12) != 0) return false;
    if (readU32BE(bytes + 18) != kRootVectorE131) return false;

    // --- Framing Layer PDU (starts at byte 38) ---
    // bytes[38..39] flags+length
    // bytes[40..43] vector = 0x00000002 (DMP data packet)
    // bytes[44..107] source name (UTF-8, 64 bytes, ignored)
    // bytes[108]    priority
    // bytes[109..110] sync universe
    // bytes[111]    sequence number
    // bytes[112]    options
    // bytes[113..114] universe (big-endian)
    if (readU32BE(bytes + kFramingStart + 2) != kFramingVectorData) return false;

    const std::uint8_t priority = bytes[kFramingStart + 70];     // 38 + 70 = 108
    const std::uint8_t sequence = bytes[kFramingStart + 73];     // 38 + 73 = 111
    const std::uint16_t universe = readU16BE(bytes + kFramingStart + 75); // 38 + 75 = 113

    // --- DMP Layer PDU (starts at byte 115) ---
    // bytes[115..116] flags+length
    // bytes[117]      vector = 0x02 (set property)
    // bytes[118]      address type & data type = 0xA1
    // bytes[119..120] first property address = 0x0000
    // bytes[121..122] address increment = 0x0001
    // bytes[123..124] property value count (BE) = 1 + channelCount
    // bytes[125]      DMX start code (usually 0x00)
    // bytes[126..]    channel values
    if (bytes[kDmpStart + 2] != kDmpVector) return false;

    const std::uint16_t propValueCount = readU16BE(bytes + kDmpStart + 8); // 115 + 8 = 123
    if (propValueCount < 1) return false;

    const std::uint16_t channelCount = static_cast<std::uint16_t>(propValueCount - 1);
    if (channelCount == 0 || channelCount > 512) return false;
    if (kPropertyValuesStart + channelCount > len) return false;

    out.universe     = universe;
    out.priority     = priority;
    out.sequence     = sequence;
    out.channelCount = channelCount;
    out.channels     = bytes + kPropertyValuesStart;
    return true;
}

} // namespace entity::dmx
