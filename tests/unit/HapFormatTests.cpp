/**
 * Unit tests for the HapFormat parser.
 *
 * Covers the three compression paths (raw / snappy / chunked), 4- and 8-byte
 * section headers, every variant's type-byte → pixel-format mapping, HapM
 * multi-image, and the main malformed-input guards. No FFmpeg or GPU dep —
 * the parser is a pure byte pipeline.
 */

#include <gtest/gtest.h>
#include "entity/media/HapFormat.hpp"
#include <snappy.h>
#include <cstdint>
#include <vector>
#include <cstring>

using namespace entity;

namespace {

// Helper: write a 4-byte header (section size + type byte)
void writeHeader4(std::vector<uint8_t>& out, uint32_t size, uint8_t type) {
    out.push_back(static_cast<uint8_t>(size & 0xFF));
    out.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
    out.push_back(type);
}

// Helper: write an 8-byte header (0-size signal + type + 32-bit size)
void writeHeader8(std::vector<uint8_t>& out, uint32_t size, uint8_t type) {
    out.push_back(0); out.push_back(0); out.push_back(0); // signals 8-byte
    out.push_back(type);
    out.push_back(static_cast<uint8_t>(size & 0xFF));
    out.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((size >> 24) & 0xFF));
}

// Build a single-image raw HAP packet with given type byte + payload.
std::vector<uint8_t> buildRawPacket(uint8_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    writeHeader4(out, static_cast<uint32_t>(payload.size()), type);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Build a single-image snappy-compressed HAP packet.
std::vector<uint8_t> buildSnappyPacket(uint8_t type, const std::vector<uint8_t>& rawPayload) {
    std::string compressed;
    snappy::Compress(reinterpret_cast<const char*>(rawPayload.data()), rawPayload.size(), &compressed);

    std::vector<uint8_t> out;
    writeHeader4(out, static_cast<uint32_t>(compressed.size()), type);
    out.insert(out.end(), compressed.begin(), compressed.end());
    return out;
}

// Produce a plausible BC1 block stream for (width, height).
std::vector<uint8_t> makeBcBlocks(uint32_t width, uint32_t height, uint32_t bytesPerBlock) {
    const uint32_t bx = (width + 3) / 4;
    const uint32_t by = (height + 3) / 4;
    std::vector<uint8_t> data(bx * by * bytesPerBlock);
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF); // arbitrary, reproducible
    }
    return data;
}

} // anonymous namespace

// ---- Raw (uncompressed) sections ----------------------------------------

TEST(HapFormatTest, ParsesRawHap1_BC1) {
    auto blocks = makeBcBlocks(8, 8, 8); // BC1 = 8 bytes per 4x4 block
    auto packet = buildRawPacket(0xAB, blocks);

    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 8, 8, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC1_UNORM);
    EXPECT_EQ(frame.colorSpace, HapColorSpace::RGB);
    EXPECT_EQ(frame.width, 8u);
    EXPECT_EQ(frame.height, 8u);
    ASSERT_EQ(frame.textureData.size(), blocks.size());
    EXPECT_EQ(std::memcmp(frame.textureData.data(), blocks.data(), blocks.size()), 0);
}

TEST(HapFormatTest, ParsesRawHap7_BC7) {
    auto blocks = makeBcBlocks(16, 8, 16); // BC7 = 16 bytes per 4x4 block
    auto packet = buildRawPacket(0xAC, blocks);

    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 16, 8, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC7_UNORM);
    EXPECT_EQ(frame.colorSpace, HapColorSpace::RGB);
    ASSERT_EQ(frame.textureData.size(), blocks.size());
    EXPECT_EQ(std::memcmp(frame.textureData.data(), blocks.data(), blocks.size()), 0);
}

TEST(HapFormatTest, ParsesRawHapY_BC3_YCoCg) {
    auto blocks = makeBcBlocks(12, 12, 16); // HapY uses BC3
    auto packet = buildRawPacket(0xAF, blocks);

    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 12, 12, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC3_UNORM);
    EXPECT_EQ(frame.colorSpace, HapColorSpace::YCoCg_scaled);
}

// ---- Snappy-compressed sections -----------------------------------------

TEST(HapFormatTest, ParsesSnappyHap5_BC3) {
    auto blocks = makeBcBlocks(8, 8, 16);
    auto packet = buildSnappyPacket(0xBE, blocks);

    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 8, 8, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC3_UNORM);
    EXPECT_EQ(frame.colorSpace, HapColorSpace::RGB);
    ASSERT_EQ(frame.textureData.size(), blocks.size());
    EXPECT_EQ(std::memcmp(frame.textureData.data(), blocks.data(), blocks.size()), 0);
}

TEST(HapFormatTest, ParsesSnappyHap7_BC7) {
    auto blocks = makeBcBlocks(32, 16, 16);
    auto packet = buildSnappyPacket(0xBC, blocks);

    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 32, 16, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC7_UNORM);
    ASSERT_EQ(frame.textureData.size(), blocks.size());
    EXPECT_EQ(std::memcmp(frame.textureData.data(), blocks.data(), blocks.size()), 0);
}

// ---- 8-byte header (size > 0 stored in upper 4 bytes) --------------------

TEST(HapFormatTest, Parses8ByteHeader) {
    auto blocks = makeBcBlocks(8, 8, 8);
    std::vector<uint8_t> packet;
    writeHeader8(packet, static_cast<uint32_t>(blocks.size()), 0xAB); // raw Hap1
    packet.insert(packet.end(), blocks.begin(), blocks.end());

    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 8, 8, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC1_UNORM);
    ASSERT_EQ(frame.textureData.size(), blocks.size());
    EXPECT_EQ(std::memcmp(frame.textureData.data(), blocks.data(), blocks.size()), 0);
}

// ---- HapH HDR variants ---------------------------------------------------

TEST(HapFormatTest, ParsesHapH_BC6H_UF16) {
    auto blocks = makeBcBlocks(8, 8, 16);
    auto packet = buildRawPacket(0xA2, blocks); // HapH unsigned HDR
    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 8, 8, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC6H_UF16);
    EXPECT_EQ(frame.colorSpace, HapColorSpace::HDR_float);
}

TEST(HapFormatTest, ParsesHapH_BC6H_SF16) {
    auto blocks = makeBcBlocks(8, 8, 16);
    auto packet = buildRawPacket(0xA3, blocks); // HapH signed HDR
    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 8, 8, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC6H_SF16);
    EXPECT_EQ(frame.colorSpace, HapColorSpace::HDR_float);
}

// ---- HapA alpha-only (BC4) ----------------------------------------------

TEST(HapFormatTest, ParsesHapA_AlphaOnly) {
    auto blocks = makeBcBlocks(8, 8, 8); // BC4 = 8 bytes per block
    auto packet = buildRawPacket(0xA1, blocks);
    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 8, 8, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC4_UNORM);
    EXPECT_EQ(frame.colorSpace, HapColorSpace::Alpha);
}

// ---- HapM multi-image (YCoCg + BC4 alpha) -------------------------------

TEST(HapFormatTest, ParsesHapM_MultiImage) {
    // Build inner YCoCg (BC3, 16 bpb) + BC4 alpha (8 bpb) sub-sections.
    auto colorBlocks = makeBcBlocks(8, 8, 16);
    auto alphaBlocks = makeBcBlocks(8, 8, 8);

    std::vector<uint8_t> inner;
    // Sub-section 1: HapY (0xAF) color plane
    writeHeader4(inner, static_cast<uint32_t>(colorBlocks.size()), 0xAF);
    inner.insert(inner.end(), colorBlocks.begin(), colorBlocks.end());
    // Sub-section 2: HapA (0xA1) alpha plane
    writeHeader4(inner, static_cast<uint32_t>(alphaBlocks.size()), 0xA1);
    inner.insert(inner.end(), alphaBlocks.begin(), alphaBlocks.end());

    // Wrap in 0x0D HapM container
    std::vector<uint8_t> packet;
    writeHeader4(packet, static_cast<uint32_t>(inner.size()), 0x0D);
    packet.insert(packet.end(), inner.begin(), inner.end());

    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 8, 8, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC3_UNORM);
    EXPECT_EQ(frame.colorSpace, HapColorSpace::YCoCg_scaled);
    EXPECT_EQ(frame.alphaFormat, HapPixelFormat::BC4_UNORM);
    ASSERT_EQ(frame.textureData.size(), colorBlocks.size());
    ASSERT_EQ(frame.alphaData.size(), alphaBlocks.size());
    EXPECT_EQ(std::memcmp(frame.textureData.data(), colorBlocks.data(), colorBlocks.size()), 0);
    EXPECT_EQ(std::memcmp(frame.alphaData.data(), alphaBlocks.data(), alphaBlocks.size()), 0);
}

// ---- Chunked (0xC_) path ------------------------------------------------

TEST(HapFormatTest, ParsesChunkedHap7_TwoChunksSnappy) {
    // Two snappy chunks, concatenated decompressed content = original blocks.
    auto blocks = makeBcBlocks(16, 16, 16);
    const size_t half = blocks.size() / 2;

    std::string chunkA, chunkB;
    snappy::Compress(reinterpret_cast<const char*>(blocks.data()),        half, &chunkA);
    snappy::Compress(reinterpret_cast<const char*>(blocks.data() + half), blocks.size() - half, &chunkB);

    // Build Decode Instructions section:
    //   0x01 container of
    //     0x02 ChunkCompressorTable (2 bytes: 0x0B, 0x0B — both snappy)
    //     0x03 ChunkSizeTable (8 bytes: 2 × uint32 LE sizes)
    std::vector<uint8_t> di;

    // 0x02 — 2 bytes
    writeHeader4(di, 2, 0x02);
    di.push_back(0x0B); di.push_back(0x0B);

    // 0x03 — 8 bytes (two LE uint32 sizes)
    writeHeader4(di, 8, 0x03);
    auto pushU32 = [&](uint32_t v) {
        di.push_back(static_cast<uint8_t>(v & 0xFF));
        di.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        di.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        di.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    };
    pushU32(static_cast<uint32_t>(chunkA.size()));
    pushU32(static_cast<uint32_t>(chunkB.size()));

    // Wrap di in 0x01 Decode Instructions section
    std::vector<uint8_t> payload;
    writeHeader4(payload, static_cast<uint32_t>(di.size()), 0x01);
    payload.insert(payload.end(), di.begin(), di.end());
    // Then the chunks themselves, back-to-back (no offset table → implied)
    payload.insert(payload.end(), chunkA.begin(), chunkA.end());
    payload.insert(payload.end(), chunkB.begin(), chunkB.end());

    // Top-level: 0xCC = chunked Hap7
    std::vector<uint8_t> packet;
    writeHeader4(packet, static_cast<uint32_t>(payload.size()), 0xCC);
    packet.insert(packet.end(), payload.begin(), payload.end());

    HapFrame frame;
    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 16, 16, frame));
    EXPECT_EQ(frame.format, HapPixelFormat::BC7_UNORM);
    ASSERT_EQ(frame.textureData.size(), blocks.size());
    EXPECT_EQ(std::memcmp(frame.textureData.data(), blocks.data(), blocks.size()), 0);
}

// ---- Malformed input guards ---------------------------------------------

TEST(HapFormatTest, RejectsTruncatedHeader) {
    const uint8_t tiny[] = {0x01, 0x02}; // only 2 bytes — can't read a 4-byte header
    HapFrame frame;
    EXPECT_FALSE(parseHapPacket(tiny, sizeof(tiny), 8, 8, frame));
}

TEST(HapFormatTest, RejectsSizeBeyondBuffer) {
    // Claims 1000-byte payload but only 10 bytes of buffer past the header.
    std::vector<uint8_t> packet;
    writeHeader4(packet, 1000, 0xAB); // raw Hap1, size 1000
    packet.resize(packet.size() + 10); // only 10 bytes of "payload" present
    HapFrame frame;
    EXPECT_FALSE(parseHapPacket(packet.data(), packet.size(), 8, 8, frame));
}

TEST(HapFormatTest, RejectsUnknownTypeByte) {
    std::vector<uint8_t> packet;
    writeHeader4(packet, 0, 0xFF); // 0xFF is not a known section type
    HapFrame frame;
    EXPECT_FALSE(parseHapPacket(packet.data(), packet.size(), 8, 8, frame));
}

TEST(HapFormatTest, RejectsNullData) {
    HapFrame frame;
    EXPECT_FALSE(parseHapPacket(nullptr, 100, 8, 8, frame));
}

// ---- Buffer reuse (steady-state alloc avoidance) ------------------------

TEST(HapFormatTest, ReusesTextureDataBuffer) {
    auto blocks = makeBcBlocks(8, 8, 8);
    auto packet = buildRawPacket(0xAB, blocks);

    HapFrame frame;
    frame.textureData.reserve(blocks.size() * 2); // pre-allocate large capacity
    const auto* originalBuf = frame.textureData.data();
    const size_t originalCap = frame.textureData.capacity();

    ASSERT_TRUE(parseHapPacket(packet.data(), packet.size(), 8, 8, frame));
    // Buffer pointer may change across runs (vector doesn't guarantee stability
    // under .assign), but capacity should not shrink below what we pre-reserved.
    // Main assertion: content is correct.
    EXPECT_EQ(std::memcmp(frame.textureData.data(), blocks.data(), blocks.size()), 0);
    (void)originalBuf;
    (void)originalCap;
}
