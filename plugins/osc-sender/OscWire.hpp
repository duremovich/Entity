// SPDX-License-Identifier: Apache-2.0
// OSC 1.0 wire-format write helpers — no Winsock, no plugin-api.
// Shared between OscSenderPlugin.cpp and unit tests.
#pragma once

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace osc {

inline void writeU32BE(std::vector<std::uint8_t>& buf, std::uint32_t v) {
    buf.push_back(std::uint8_t(v >> 24));
    buf.push_back(std::uint8_t(v >> 16));
    buf.push_back(std::uint8_t(v >>  8));
    buf.push_back(std::uint8_t(v      ));
}

inline void writeI32BE(std::vector<std::uint8_t>& buf, std::int32_t v) {
    writeU32BE(buf, static_cast<std::uint32_t>(v));
}

inline void writeF32BE(std::vector<std::uint8_t>& buf, float v) {
    std::uint32_t bits;
    std::memcpy(&bits, &v, sizeof(bits));
    writeU32BE(buf, bits);
}

// Null-terminated, padded to 4-byte boundary.
inline void writeOscString(std::vector<std::uint8_t>& buf, std::string_view s) {
    for (char c : s) buf.push_back(static_cast<std::uint8_t>(c));
    buf.push_back(0);
    std::size_t len = s.size() + 1;
    std::size_t padded = (len + 3u) & ~std::size_t(3);
    for (std::size_t i = len; i < padded; ++i) buf.push_back(0);
}

inline void buildOscMessageInt(std::vector<std::uint8_t>& out,
                                std::string_view address, std::int32_t value) {
    out.clear();
    writeOscString(out, address);
    writeOscString(out, ",i");
    writeI32BE(out, value);
}

inline void buildOscMessageFloat(std::vector<std::uint8_t>& out,
                                  std::string_view address, float value) {
    out.clear();
    writeOscString(out, address);
    writeOscString(out, ",f");
    writeF32BE(out, value);
}

inline void buildOscMessageString(std::vector<std::uint8_t>& out,
                                   std::string_view address, std::string_view value) {
    out.clear();
    writeOscString(out, address);
    writeOscString(out, ",s");
    writeOscString(out, value);
}

inline void writeBundleHeader(std::vector<std::uint8_t>& buf) {
    static const char kTag[8] = {'#','b','u','n','d','l','e','\0'};
    for (char c : kTag) buf.push_back(static_cast<std::uint8_t>(c));
    writeU32BE(buf, 0x00000000u); // timetag hi: immediate
    writeU32BE(buf, 0x00000001u); // timetag lo: immediate
}

inline void appendBundleMessage(std::vector<std::uint8_t>& bundle,
                                 const std::vector<std::uint8_t>& msg) {
    writeU32BE(bundle, static_cast<std::uint32_t>(msg.size()));
    bundle.insert(bundle.end(), msg.begin(), msg.end());
}

} // namespace osc
