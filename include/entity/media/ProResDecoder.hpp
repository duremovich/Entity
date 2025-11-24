#pragma once

#include "entity/media/Decoder.hpp"

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace entity {

/**
 * ProResDecoder - Decoder for Apple ProRes 4444 codec.
 *
 * Supports ProRes 4444 with alpha channel for transparency.
 * Uses FFmpeg for decoding and swscale for pixel format conversion.
 *
 * ProRes 4444 Features:
 * - High quality, visually lossless
 * - Alpha channel support (4444 variant)
 * - Widely used in professional video production
 * - Optimized for real-time playback
 *
 * Thread Safety:
 * - NOT thread-safe by design
 * - Each decoder instance should be used by a single decode thread
 * - DecodeSystem manages decoder instances and threading
 *
 * FFmpeg Integration:
 * - Handles YUV422P10LE (ProRes native) to RGBA conversion
 * - Premultiplies alpha channel during conversion
 * - Manages FFmpeg context lifetimes (RAII pattern)
 */
class ProResDecoder : public Decoder {
public:
    ProResDecoder();
    ~ProResDecoder() override;

    // Decoder interface implementation
    Result open(const std::string& filepath) override;
    void close() override;
    Result decodeFrame(FrameNumber frameNumber, DecodedFrame& outFrame) override;
    Result seek(FrameNumber frameNumber) override;

    // Media properties
    MediaType getMediaType() const override { return MediaType::VideoProRes4444; }
    uint32_t getWidth() const override { return m_width; }
    uint32_t getHeight() const override { return m_height; }
    double getFrameRate() const override { return m_frameRate; }
    FrameNumber getDuration() const override { return m_duration; }
    bool hasAlpha() const override { return m_hasAlpha; }
    bool isOpen() const override { return m_isOpen; }
    const std::string& getFilePath() const override { return m_filepath; }

private:
    /**
     * Convert decoded AVFrame to RGBA with premultiplied alpha.
     * Handles conversion from YUV422P10LE (ProRes native format) to RGBA8.
     *
     * @param srcFrame Source frame from FFmpeg (YUV422P10LE)
     * @param outFrame Output frame to receive RGBA data
     * @return Result::Success on success, error code otherwise
     */
    Result convertToRGBA(AVFrame* srcFrame, DecodedFrame& outFrame);

    /**
     * Read and decode the next packet.
     * Continues decoding until a complete frame is available.
     *
     * @return Result::Success when frame decoded, error code otherwise
     */
    Result decodeNextPacket();

    /**
     * Cleanup FFmpeg resources.
     * Called during close() to properly deallocate all FFmpeg contexts.
     */
    void cleanupFFmpeg();

private:
    // File information
    std::string m_filepath;
    bool m_isOpen{false};

    // Media properties
    uint32_t m_width{0};
    uint32_t m_height{0};
    double m_frameRate{30.0};
    FrameNumber m_duration{0};
    bool m_hasAlpha{false};

    // FFmpeg contexts (nullptr when FFmpeg not available)
    // Forward declared types to avoid FFmpeg header dependency
    AVFormatContext* m_formatContext{nullptr};
    AVCodecContext* m_codecContext{nullptr};
    AVFrame* m_frame{nullptr};
    AVPacket* m_packet{nullptr};
    SwsContext* m_swsContext{nullptr};

    // Stream tracking
    int m_videoStreamIndex{-1};
    FrameNumber m_currentFrame{-1};
};

} // namespace entity
