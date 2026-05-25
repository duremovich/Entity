#pragma once

#include "entity/media/Decoder.hpp"
#include <vector>
#include <cstdint>

namespace entity {

/**
 * PNGSequenceDecoder - Decoder for PNG image sequences.
 *
 * Loads numbered PNG sequences like:
 * - image_001.png, image_002.png, ...
 * - frame001.png, frame002.png, ...
 *
 * Features:
 * - Supports alpha channel
 * - No codec dependencies (uses stb_image)
 * - Simple frame access (just file reads)
 * - Cache-friendly frame loading
 *
 * Performance Note:
 * - PNG decoding is CPU-intensive
 * - Frames are decoded on-demand (not buffered)
 * - Consider caching or using compressed video for large sequences
 *
 * Thread Safety:
 * - NOT thread-safe by design
 * - Each decoder instance should be used by a single decode thread
 * - DecodeSystem manages decoder instances and threading
 */
class PNGSequenceDecoder : public Decoder {
public:
    PNGSequenceDecoder();
    ~PNGSequenceDecoder() override;

    // Decoder interface implementation
    Result open(const std::string& filepath) override;
    void close() override;
    Result decodeFrame(FrameNumber frameNumber, DecodedFrame& outFrame) override;
    Result seek(FrameNumber frameNumber) override;
    void setAllocator(IDecodeBufferAllocator* allocator) override { m_allocator = allocator; }

    // Media properties
    MediaType getMediaType() const override { return MediaType::PNGSequence; }
    uint32_t getWidth() const override { return m_width; }
    uint32_t getHeight() const override { return m_height; }
    double getFrameRate() const override { return m_frameRate; }
    FrameNumber getDuration() const override { return m_duration; }
    bool hasAlpha() const override { return m_hasAlpha; }
    bool isOpen() const override { return m_isOpen; }
    const std::string& getFilePath() const override { return m_basePath; }

private:
    /**
     * Scan directory for PNG files and build file list.
     * Sorts files alphabetically to determine sequence order.
     *
     * @return Result::Success on success, error code otherwise
     */
    Result scanDirectory();

    /**
     * Load a PNG file and convert to RGBA with premultiplied alpha.
     * Uses stb_image to load PNG and converts to RGBA format.
     *
     * @param filepath Full path to PNG file
     * @param outFrame Output frame to receive RGBA data
     * @return Result::Success on success, error code otherwise
     */
    Result loadPNG(const std::string& filepath, DecodedFrame& outFrame);

    /**
     * Premultiply alpha channel in RGBA data.
     * Converts straight alpha to premultiplied alpha for GPU rendering.
     *
     * @param rgba Pointer to RGBA data (width * height * 4 bytes)
     * @param width Image width in pixels
     * @param height Image height in pixels
     */
    void premultiplyAlpha(uint8_t* rgba, uint32_t width, uint32_t height);

private:
    // Buffer allocator — set by DecodeSystem::createWorker via setAllocator().
    // Null until wired; loadPNG falls back to outFrame.allocate() when null.
    IDecodeBufferAllocator* m_allocator{nullptr};

    std::string m_basePath;     // Base directory containing PNG files
    std::vector<std::string> m_fileList;  // List of PNG files in sequence order
    bool m_isOpen{false};

    // Media properties
    uint32_t m_width{0};
    uint32_t m_height{0};
    double m_frameRate{30.0};  // Default 30 fps, customizable by user
    FrameNumber m_duration{0}; // Number of frames in sequence
    bool m_hasAlpha{true};     // PNG may or may not have alpha channel

    // State tracking
    FrameNumber m_currentFrame{-1};
};

} // namespace entity
