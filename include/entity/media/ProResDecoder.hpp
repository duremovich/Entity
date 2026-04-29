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
     * Read the next packet, optionally decoding it.
     *
     * When `actuallyDecode == true` (the default, sequential path), reads
     * the next video packet, sends it through avcodec, receives a decoded
     * AVFrame, and increments m_currentFrame on success.
     *
     * When `actuallyDecode == false`, reads + unrefs the next video packet
     * WITHOUT sending it to avcodec, and increments m_currentFrame anyway.
     * Used by `decodeFrame()` to cheaply skip intermediate frames when
     * jumping forward to a target. **Safe only because ProRes is intra-
     * only**: each packet is a complete keyframe, no inter-frame
     * references, so the decoder context's state isn't disturbed by
     * skipping packets.
     *
     * @return Result::Success when packet processed (decoded or skipped),
     *         error code otherwise. EndOfStream propagates as today.
     */
    Result decodeNextPacket(bool actuallyDecode = true);

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
