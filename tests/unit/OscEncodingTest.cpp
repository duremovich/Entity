// OSC 1.0 wire-format encoding tests.
// Exercises OscWire.hpp (sender encode helpers) and the read helpers from
// OscInboundMappings.hpp (receiver side) to verify parity: bytes written by
// the sender are correctly decoded by the receiver's read path.

#include "../../plugins/osc-sender/OscWire.hpp"
#include "../../plugins/osc-receiver/OscInboundMappings.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace osc;
using namespace osc_inbound;

// ---------------------------------------------------------------------------
// writeI32BE

TEST(OscEncoding, WriteI32BE_Zero) {
    std::vector<std::uint8_t> buf;
    writeI32BE(buf, 0);
    ASSERT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf[0], 0); EXPECT_EQ(buf[1], 0);
    EXPECT_EQ(buf[2], 0); EXPECT_EQ(buf[3], 0);
}

TEST(OscEncoding, WriteI32BE_One) {
    std::vector<std::uint8_t> buf;
    writeI32BE(buf, 1);
    ASSERT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf[0], 0); EXPECT_EQ(buf[1], 0);
    EXPECT_EQ(buf[2], 0); EXPECT_EQ(buf[3], 1);
}

TEST(OscEncoding, WriteI32BE_MinusOne) {
    std::vector<std::uint8_t> buf;
    writeI32BE(buf, -1);
    ASSERT_EQ(buf.size(), 4u);
    // -1 in two's complement = 0xFFFFFFFF
    EXPECT_EQ(buf[0], 0xFF); EXPECT_EQ(buf[1], 0xFF);
    EXPECT_EQ(buf[2], 0xFF); EXPECT_EQ(buf[3], 0xFF);
}

TEST(OscEncoding, WriteI32BE_Max) {
    std::vector<std::uint8_t> buf;
    writeI32BE(buf, INT32_MAX); // 0x7FFFFFFF
    ASSERT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf[0], 0x7F); EXPECT_EQ(buf[1], 0xFF);
    EXPECT_EQ(buf[2], 0xFF); EXPECT_EQ(buf[3], 0xFF);
}

TEST(OscEncoding, WriteI32BE_Min) {
    std::vector<std::uint8_t> buf;
    writeI32BE(buf, INT32_MIN); // 0x80000000
    ASSERT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf[0], 0x80); EXPECT_EQ(buf[1], 0x00);
    EXPECT_EQ(buf[2], 0x00); EXPECT_EQ(buf[3], 0x00);
}

// Read back via the receiver helper must round-trip.
TEST(OscEncoding, I32BE_RoundTrip) {
    for (std::int32_t v : {0, 1, -1, INT32_MAX, INT32_MIN, 42, -100}) {
        std::vector<std::uint8_t> buf;
        writeI32BE(buf, v);
        EXPECT_EQ(readI32BE(buf.data()), v) << "value=" << v;
    }
}

// ---------------------------------------------------------------------------
// writeF32BE

TEST(OscEncoding, F32BE_RoundTrip) {
    for (float v : {0.0f, 1.0f, -1.0f, 3.14159f, 1e10f, -1e-5f}) {
        std::vector<std::uint8_t> buf;
        writeF32BE(buf, v);
        ASSERT_EQ(buf.size(), 4u);
        float back = readF32BE(buf.data());
        // Bit-exact round-trip (no conversion involved).
        std::uint32_t vBits, backBits;
        std::memcpy(&vBits, &v, 4);
        std::memcpy(&backBits, &back, 4);
        EXPECT_EQ(vBits, backBits) << "value=" << v;
    }
}

// ---------------------------------------------------------------------------
// writeOscString padding

TEST(OscEncoding, OscString_EmptyPadsTo4) {
    std::vector<std::uint8_t> buf;
    writeOscString(buf, "");
    // "" + NUL = 1 byte → padded to 4
    ASSERT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf[0], 0);
    EXPECT_EQ(buf[1], 0);
    EXPECT_EQ(buf[2], 0);
    EXPECT_EQ(buf[3], 0);
}

TEST(OscEncoding, OscString_3CharsTo8) {
    std::vector<std::uint8_t> buf;
    writeOscString(buf, "abc"); // 3 + NUL = 4 → already aligned
    ASSERT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf[0], 'a'); EXPECT_EQ(buf[1], 'b');
    EXPECT_EQ(buf[2], 'c'); EXPECT_EQ(buf[3], 0);
}

TEST(OscEncoding, OscString_4CharsTo8) {
    std::vector<std::uint8_t> buf;
    writeOscString(buf, "abcd"); // 4 + NUL = 5 → padded to 8
    ASSERT_EQ(buf.size(), 8u);
    EXPECT_EQ(buf[4], 0); // NUL
    EXPECT_EQ(buf[5], 0); // pad
    EXPECT_EQ(buf[6], 0); // pad
    EXPECT_EQ(buf[7], 0); // pad
}

TEST(OscEncoding, OscString_7CharsTo8) {
    std::vector<std::uint8_t> buf;
    writeOscString(buf, "1234567"); // 7 + NUL = 8 → no extra pad
    ASSERT_EQ(buf.size(), 8u);
}

TEST(OscEncoding, OscString_8CharsTo12) {
    std::vector<std::uint8_t> buf;
    writeOscString(buf, "12345678"); // 8 + NUL = 9 → padded to 12
    ASSERT_EQ(buf.size(), 12u);
}

// The receiver's readOscString (reproduced inline) must see the same string back.
// We test by building a message and verifying the address field with a manual parse.
TEST(OscEncoding, OscString_ReadBackViaReceiverRules) {
    // readOscString from OscReceiverPlugin.cpp: reads until NUL, then rounds
    // pos up to 4-byte boundary. Mirror that logic here.
    auto parseStr = [](const std::vector<std::uint8_t>& buf, std::size_t& pos) -> std::string {
        std::size_t start = pos;
        while (pos < buf.size() && buf[pos] != 0) ++pos;
        std::string s(buf.begin() + start, buf.begin() + pos);
        ++pos; // past NUL
        pos = (pos + 3u) & ~std::size_t(3);
        return s;
    };

    std::vector<std::uint8_t> buf;
    writeOscString(buf, "/entity/play");
    std::size_t pos = 0;
    std::string back = parseStr(buf, pos);
    EXPECT_EQ(back, "/entity/play");
    EXPECT_EQ(pos, buf.size()); // consumed exactly
}

// ---------------------------------------------------------------------------
// Full message round-trip via buildOscMessageInt

TEST(OscEncoding, BuildOscMessageInt_RoundTrip) {
    std::vector<std::uint8_t> msg;
    buildOscMessageInt(msg, "/entity/out/transport/frame", 42);

    // Parse: address string, type-tag string, int32 arg.
    auto parseStr = [](const std::vector<std::uint8_t>& buf, std::size_t& pos) -> std::string {
        std::size_t start = pos;
        while (pos < buf.size() && buf[pos] != 0) ++pos;
        std::string s(buf.begin() + start, buf.begin() + pos);
        ++pos;
        pos = (pos + 3u) & ~std::size_t(3);
        return s;
    };

    std::size_t pos = 0;
    std::string addr = parseStr(msg, pos);
    std::string tag  = parseStr(msg, pos);
    EXPECT_EQ(addr, "/entity/out/transport/frame");
    EXPECT_EQ(tag,  ",i");
    ASSERT_GE(msg.size(), pos + 4);
    EXPECT_EQ(readI32BE(msg.data() + pos), 42);
}

// ---------------------------------------------------------------------------
// Bundle: build + parse two messages

TEST(OscEncoding, Bundle_TwoMessages) {
    std::vector<std::uint8_t> m1, m2;
    buildOscMessageInt(m1, "/entity/out/transport/frame", 100);
    buildOscMessageString(m2, "/entity/out/transport/state", "playing");

    std::vector<std::uint8_t> bundle;
    writeBundleHeader(bundle);
    appendBundleMessage(bundle, m1);
    appendBundleMessage(bundle, m2);

    // Verify #bundle header.
    ASSERT_GE(bundle.size(), 16u);
    EXPECT_EQ(bundle[0], '#');
    EXPECT_EQ(bundle[1], 'b');
    EXPECT_EQ(bundle[2], 'u');
    EXPECT_EQ(bundle[3], 'n');
    EXPECT_EQ(bundle[4], 'd');
    EXPECT_EQ(bundle[5], 'l');
    EXPECT_EQ(bundle[6], 'e');
    EXPECT_EQ(bundle[7], 0);
    // Timetag = immediate = 0x0000000000000001
    EXPECT_EQ(readI32BE(bundle.data() + 8),  0);
    EXPECT_EQ(readI32BE(bundle.data() + 12), 1);

    // First element length + content.
    std::size_t pos = 16;
    std::uint32_t len1 = readU32BE(bundle.data() + pos); pos += 4;
    EXPECT_EQ(len1, m1.size());
    // The element bytes should match m1 exactly.
    EXPECT_EQ(std::memcmp(bundle.data() + pos, m1.data(), m1.size()), 0);
    pos += len1;

    // Second element.
    std::uint32_t len2 = readU32BE(bundle.data() + pos); pos += 4;
    EXPECT_EQ(len2, m2.size());
    EXPECT_EQ(std::memcmp(bundle.data() + pos, m2.data(), m2.size()), 0);
    pos += len2;

    EXPECT_EQ(pos, bundle.size());
}
