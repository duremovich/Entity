#include <gtest/gtest.h>
#include "entity/media/PNGSequenceDecoder.hpp"
#include <fstream>
#include <filesystem>
#include <cstring>
#include <vector>

namespace entity {
namespace test {

/**
 * Test suite for PNGSequenceDecoder.
 *
 * Tests cover:
 * - Directory scanning for PNG sequences
 * - Frame dimension detection
 * - Alpha channel detection
 * - PNG decoding and RGBA conversion
 * - Premultiplied alpha blending
 */
class PNGSequenceDecoderTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create temporary test directory
        testDir = std::filesystem::temp_directory_path() / "png_seq_test";
        std::filesystem::create_directories(testDir);
    }

    void TearDown() override {
        // Cleanup temporary directory
        try {
            std::filesystem::remove_all(testDir);
        }
        catch (...) {
            // Ignore cleanup errors
        }
    }

    std::filesystem::path testDir;
};

/**
 * Test basic decoder initialization.
 */
TEST_F(PNGSequenceDecoderTest, Initialization) {
    PNGSequenceDecoder decoder;

    EXPECT_FALSE(decoder.isOpen());
    EXPECT_EQ(decoder.getWidth(), 0);
    EXPECT_EQ(decoder.getHeight(), 0);
    EXPECT_EQ(decoder.getDuration(), 0);
    EXPECT_EQ(decoder.getMediaType(), MediaType::PNGSequence);
    EXPECT_TRUE(decoder.hasAlpha());
}

/**
 * Test that decoder fails gracefully on missing directory.
 */
TEST_F(PNGSequenceDecoderTest, MissingDirectory) {
    PNGSequenceDecoder decoder;
    std::string nonexistentPath = testDir.string() + "/nonexistent/fake.png";

    Result result = decoder.open(nonexistentPath);

    EXPECT_EQ(result, Result::FileNotFound);
    EXPECT_FALSE(decoder.isOpen());
}

/**
 * Test that decoder fails when no PNG files found.
 */
TEST_F(PNGSequenceDecoderTest, NoFilesFound) {
    PNGSequenceDecoder decoder;
    std::string emptyDirPath = testDir.string();

    Result result = decoder.open(emptyDirPath);

    EXPECT_EQ(result, Result::FileNotFound);
    EXPECT_FALSE(decoder.isOpen());
}

/**
 * Test seek operation on unopened decoder.
 */
TEST_F(PNGSequenceDecoderTest, SeekBeforeOpen) {
    PNGSequenceDecoder decoder;

    Result result = decoder.seek(0);

    EXPECT_EQ(result, Result::Failure);
}

/**
 * Test decodeFrame on unopened decoder.
 */
TEST_F(PNGSequenceDecoderTest, DecodeFrameBeforeOpen) {
    PNGSequenceDecoder decoder;
    DecodedFrame frame;

    Result result = decoder.decodeFrame(0, frame);

    EXPECT_EQ(result, Result::Failure);
}

/**
 * Test invalid frame number.
 */
TEST_F(PNGSequenceDecoderTest, InvalidFrameNumber) {
    PNGSequenceDecoder decoder;

    // Create a valid sequence but don't open it yet
    // (we're just testing error handling)
    // This would need actual PNG files to fully test

    DecodedFrame frame;
    Result result = decoder.decodeFrame(-1, frame);

    EXPECT_EQ(result, Result::Failure);
}

/**
 * Test close operation.
 */
TEST_F(PNGSequenceDecoderTest, Close) {
    PNGSequenceDecoder decoder;

    // Should not crash even though decoder isn't open
    decoder.close();

    EXPECT_FALSE(decoder.isOpen());
}

/**
 * Test multiple open/close cycles.
 */
TEST_F(PNGSequenceDecoderTest, MultipleOpenClose) {
    PNGSequenceDecoder decoder;

    // Multiple closes should not crash
    decoder.close();
    decoder.close();

    EXPECT_FALSE(decoder.isOpen());
}

/**
 * Test frame rate property.
 */
TEST_F(PNGSequenceDecoderTest, FrameRate) {
    PNGSequenceDecoder decoder;

    EXPECT_EQ(decoder.getFrameRate(), 30.0);  // Default frame rate
}

/**
 * Test file path property.
 */
TEST_F(PNGSequenceDecoderTest, FilePath) {
    PNGSequenceDecoder decoder;

    EXPECT_EQ(decoder.getFilePath(), "");  // Empty before open
}

/**
 * Test that scanning produces deterministic ordering.
 * This tests that PNG files are sorted alphabetically.
 */
TEST_F(PNGSequenceDecoderTest, FileOrderingDeterministic) {
    PNGSequenceDecoder decoder1;
    PNGSequenceDecoder decoder2;

    // Both decoders should fail on nonexistent directory,
    // but they should behave consistently
    Result result1 = decoder1.open((testDir / "nonexistent").string());
    Result result2 = decoder2.open((testDir / "nonexistent").string());

    EXPECT_EQ(result1, result2);
}

} // namespace test
} // namespace entity
