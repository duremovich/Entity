#include <gtest/gtest.h>
#include "entity/media/Decoder.hpp"

using namespace entity;

// Test suite for Decoder utility functions
class DecoderUtilsTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// Test media type detection from file extensions
TEST_F(DecoderUtilsTest, DetectMediaType_ProResFromMOV) {
    MediaType type = detectMediaType("test.mov");
    EXPECT_EQ(type, MediaType::VideoProRes4444);
}

TEST_F(DecoderUtilsTest, DetectMediaType_ProResFromMP4) {
    MediaType type = detectMediaType("test.mp4");
    EXPECT_EQ(type, MediaType::VideoProRes4444);
}

TEST_F(DecoderUtilsTest, DetectMediaType_ProResFromM4V) {
    MediaType type = detectMediaType("test.m4v");
    EXPECT_EQ(type, MediaType::VideoProRes4444);
}

TEST_F(DecoderUtilsTest, DetectMediaType_HAPFromAVI) {
    MediaType type = detectMediaType("test.avi");
    EXPECT_EQ(type, MediaType::VideoHAP);
}

TEST_F(DecoderUtilsTest, DetectMediaType_PNGSequence) {
    MediaType type = detectMediaType("frame_0001.png");
    EXPECT_EQ(type, MediaType::PNGSequence);
}

TEST_F(DecoderUtilsTest, DetectMediaType_DPXSequence) {
    MediaType type = detectMediaType("frame_0001.dpx");
    EXPECT_EQ(type, MediaType::DPXSequence);
}

TEST_F(DecoderUtilsTest, DetectMediaType_UnknownExtension) {
    MediaType type = detectMediaType("test.xyz");
    EXPECT_EQ(type, MediaType::Unknown);
}

TEST_F(DecoderUtilsTest, DetectMediaType_NoExtension) {
    MediaType type = detectMediaType("test");
    EXPECT_EQ(type, MediaType::Unknown);
}

TEST_F(DecoderUtilsTest, DetectMediaType_CaseInsensitive) {
    EXPECT_EQ(detectMediaType("TEST.MOV"), MediaType::VideoProRes4444);
    EXPECT_EQ(detectMediaType("TEST.Mov"), MediaType::VideoProRes4444);
    EXPECT_EQ(detectMediaType("TEST.PNG"), MediaType::PNGSequence);
    EXPECT_EQ(detectMediaType("TEST.Png"), MediaType::PNGSequence);
}

TEST_F(DecoderUtilsTest, DetectMediaType_WithFullPath) {
    MediaType type = detectMediaType("C:\\Users\\Videos\\project\\clip.mov");
    EXPECT_EQ(type, MediaType::VideoProRes4444);
}

TEST_F(DecoderUtilsTest, DetectMediaType_UnixPath) {
    MediaType type = detectMediaType("/home/user/videos/clip.mov");
    EXPECT_EQ(type, MediaType::VideoProRes4444);
}

// Test decoder factory function
TEST_F(DecoderUtilsTest, CreateDecoder_CreatesDecoderInstances) {
    // ProResDecoder
    auto decoder = createDecoder(MediaType::VideoProRes4444);
    EXPECT_NE(decoder, nullptr);

    // PNGSequenceDecoder
    decoder = createDecoder(MediaType::PNGSequence);
    EXPECT_NE(decoder, nullptr);
}

TEST_F(DecoderUtilsTest, CreateDecoder_HAPReturnsInstance) {
    // Phase C #6 — HAP decoder now constructs. No open() here, just verify
    // the factory returns a real instance for every HAP variant.
    EXPECT_NE(createDecoder(MediaType::VideoHAP), nullptr);
    EXPECT_NE(createDecoder(MediaType::VideoHAPAlpha), nullptr);
    EXPECT_NE(createDecoder(MediaType::VideoHAPQ), nullptr);
    EXPECT_NE(createDecoder(MediaType::VideoHAPR), nullptr);
}

// Mock decoder for interface testing
class MockDecoder : public Decoder {
public:
    MockDecoder() = default;

    Result open(const std::string& filepath) override {
        m_filepath = filepath;
        m_isOpen = true;
        return Result::Success;
    }

    void close() override {
        m_isOpen = false;
        m_filepath.clear();
    }

    Result decodeFrame(FrameNumber frameNumber, DecodedFrame& outFrame) override {
        if (!m_isOpen) return Result::DecoderError;
        if (frameNumber < 0 || frameNumber >= m_duration) return Result::InvalidParameter;

        // Create mock frame
        outFrame.allocate(m_width, m_height);
        outFrame.frameNumber = frameNumber;
        outFrame.valid = true;
        return Result::Success;
    }

    Result seek(FrameNumber frameNumber) override {
        if (!m_isOpen) return Result::DecoderError;
        if (frameNumber < 0 || frameNumber >= m_duration) return Result::InvalidParameter;
        m_currentFrame = frameNumber;
        return Result::Success;
    }

    MediaType getMediaType() const override { return m_mediaType; }
    uint32_t getWidth() const override { return m_width; }
    uint32_t getHeight() const override { return m_height; }
    double getFrameRate() const override { return m_frameRate; }
    FrameNumber getDuration() const override { return m_duration; }
    bool hasAlpha() const override { return m_hasAlpha; }
    bool isOpen() const override { return m_isOpen; }
    const std::string& getFilePath() const override { return m_filepath; }

    // Test helpers
    void setMediaType(MediaType type) { m_mediaType = type; }
    void setDimensions(uint32_t w, uint32_t h) { m_width = w; m_height = h; }
    void setFrameRate(double fps) { m_frameRate = fps; }
    void setDuration(FrameNumber frames) { m_duration = frames; }
    void setHasAlpha(bool alpha) { m_hasAlpha = alpha; }

private:
    std::string m_filepath;
    bool m_isOpen{false};
    MediaType m_mediaType{MediaType::Unknown};
    uint32_t m_width{1920};
    uint32_t m_height{1080};
    double m_frameRate{30.0};
    FrameNumber m_duration{100};
    bool m_hasAlpha{false};
    FrameNumber m_currentFrame{0};
};

// Test suite for Decoder interface
class DecoderInterfaceTest : public ::testing::Test {
protected:
    void SetUp() override {
        decoder = std::make_unique<MockDecoder>();
    }

    void TearDown() override {
        decoder.reset();
    }

    std::unique_ptr<MockDecoder> decoder;
};

TEST_F(DecoderInterfaceTest, InitialState) {
    EXPECT_FALSE(decoder->isOpen());
    EXPECT_EQ(decoder->getFilePath(), "");
}

TEST_F(DecoderInterfaceTest, OpenClose) {
    Result result = decoder->open("test.mov");
    EXPECT_EQ(result, Result::Success);
    EXPECT_TRUE(decoder->isOpen());
    EXPECT_EQ(decoder->getFilePath(), "test.mov");

    decoder->close();
    EXPECT_FALSE(decoder->isOpen());
}

TEST_F(DecoderInterfaceTest, GetMediaProperties) {
    decoder->setDimensions(3840, 2160);
    decoder->setFrameRate(60.0);
    decoder->setDuration(1800);
    decoder->setHasAlpha(true);
    decoder->setMediaType(MediaType::VideoProRes4444);

    EXPECT_EQ(decoder->getWidth(), 3840);
    EXPECT_EQ(decoder->getHeight(), 2160);
    EXPECT_DOUBLE_EQ(decoder->getFrameRate(), 60.0);
    EXPECT_EQ(decoder->getDuration(), 1800);
    EXPECT_TRUE(decoder->hasAlpha());
    EXPECT_EQ(decoder->getMediaType(), MediaType::VideoProRes4444);
}

TEST_F(DecoderInterfaceTest, DecodeFrame_Success) {
    decoder->open("test.mov");
    decoder->setDuration(100);

    DecodedFrame frame;
    Result result = decoder->decodeFrame(50, frame);

    EXPECT_EQ(result, Result::Success);
    EXPECT_EQ(frame.frameNumber, 50);
    EXPECT_TRUE(frame.valid.load(std::memory_order_acquire));
    EXPECT_EQ(frame.width, decoder->getWidth());
    EXPECT_EQ(frame.height, decoder->getHeight());
}

TEST_F(DecoderInterfaceTest, DecodeFrame_NotOpen) {
    DecodedFrame frame;
    Result result = decoder->decodeFrame(0, frame);
    EXPECT_EQ(result, Result::DecoderError);
}

TEST_F(DecoderInterfaceTest, DecodeFrame_InvalidFrameNumber) {
    decoder->open("test.mov");
    decoder->setDuration(100);

    DecodedFrame frame;

    // Negative frame number
    Result result = decoder->decodeFrame(-1, frame);
    EXPECT_EQ(result, Result::InvalidParameter);

    // Frame number beyond duration
    result = decoder->decodeFrame(100, frame);
    EXPECT_EQ(result, Result::InvalidParameter);
}

TEST_F(DecoderInterfaceTest, Seek_Success) {
    decoder->open("test.mov");
    decoder->setDuration(100);

    Result result = decoder->seek(50);
    EXPECT_EQ(result, Result::Success);
}

TEST_F(DecoderInterfaceTest, Seek_NotOpen) {
    Result result = decoder->seek(0);
    EXPECT_EQ(result, Result::DecoderError);
}

TEST_F(DecoderInterfaceTest, Seek_InvalidFrameNumber) {
    decoder->open("test.mov");
    decoder->setDuration(100);

    Result result = decoder->seek(-1);
    EXPECT_EQ(result, Result::InvalidParameter);

    result = decoder->seek(100);
    EXPECT_EQ(result, Result::InvalidParameter);
}
