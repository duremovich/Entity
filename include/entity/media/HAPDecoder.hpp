#pragma once

#include "entity/media/Decoder.hpp"

// Forward declarations
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace entity {

/**
 * HAPDecoder - Decoder for HAP codec variants.
 *
 * Supports HAP, HAP Alpha, and HAP Q variants.
 * HAP uses DXT texture compression which can be uploaded directly to GPU.
 *
 * HAP Variants:
 * - HAP: DXT1 compression (no alpha)
 * - HAP Alpha: DXT5 compression (with alpha)
 * - HAP Q: Higher quality variant
 *
 * Performance Benefits:
 * - Frames compressed on disk remain compressed in memory
 * - Can upload directly to GPU without decompression (in future phases)
 * - Lower memory bandwidth requirements
 *
 * Thread Safety:
 * - NOT thread-safe: Decoders are NOT thread-safe by design
 * - Each decoder instance should be used by a single decode thread
 * - DecodeSystem manages decoder instances and threading
 */
class HAPDecoder : public Decoder {
public:
    HAPDecoder();
    ~HAPDecoder() override;

    // Decoder interface
    Result open(const std::string& filepath) override;
    void close() override;
    Result decodeFrame(FrameNumber frameNumber, DecodedFrame& outFrame) override;
    Result seek(FrameNumber frameNumber) override;

    // Media properties
    MediaType getMediaType() const override { return m_mediaType; }
    uint32_t getWidth() const override { return m_width; }
    uint32_t getHeight() const override { return m_height; }
    double getFrameRate() const override { return m_frameRate; }
    FrameNumber getDuration() const override { return m_duration; }
    bool hasAlpha() const override { return m_hasAlpha; }
    bool isOpen() const override { return m_isOpen; }
    const std::string& getFilePath() const override { return m_filepath; }

private:
    /**
     * Detect which HAP variant is used.
     * Examines codec context to distinguish between HAP, HAP Alpha, and HAP Q.
     *
     * @return Result::Success if variant detected, error code otherwise
     */
    Result detectHAPVariant();

    /**
     * Convert HAP compressed frame to RGBA.
     *
     * HAP frames come in DXT-compressed format. This method decompresses
     * them to RGBA for rendering (premultiplied alpha for HAP Alpha).
     *
     * Future optimization: Could upload DXT directly to GPU without decompression.
     *
     * @param srcFrame FFmpeg frame with HAP data
     * @param outFrame Output frame to receive RGBA data
     * @return Result::Success on success, error code otherwise
     */
    Result convertToRGBA(AVFrame* srcFrame, DecodedFrame& outFrame);

private:
    // File and format info
    std::string m_filepath;
    bool m_isOpen{false};
    MediaType m_mediaType{MediaType::VideoHAP};

    // Video metadata
    uint32_t m_width{0};
    uint32_t m_height{0};
    double m_frameRate{30.0};
    FrameNumber m_duration{0};
    bool m_hasAlpha{false};

    // FFmpeg resources (may be nullptr if HAVE_FFMPEG not defined)
    AVFormatContext* m_formatContext{nullptr};
    AVCodecContext* m_codecContext{nullptr};
    AVFrame* m_frame{nullptr};
    AVPacket* m_packet{nullptr};

    // Stream info
    int m_videoStreamIndex{-1};

    // Decoder state
    FrameNumber m_currentFrame{-1};
};

} // namespace entity
