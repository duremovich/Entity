#pragma once

#include "entity/core/Types.hpp"
#include "entity/media/DecodedFrame.hpp"
#include <string>
#include <memory>

namespace entity {

/**
 * Decoder - Abstract base class for all media decoders.
 *
 * Defines the interface that all codec-specific decoders must implement.
 * Decoders are responsible for opening media files, seeking to frames,
 * and decoding video frames into RGBA format for rendering.
 *
 * Thread Safety:
 * - Decoders are NOT thread-safe by design
 * - Each decoder instance should be used by a single decode thread
 * - DecodeSystem manages decoder instances and threading
 */
class Decoder {
public:
    virtual ~Decoder() = default;

    /**
     * Open a media file and initialize the decoder.
     *
     * @param filepath Path to media file
     * @return Result::Success on success, error code otherwise
     */
    virtual Result open(const std::string& filepath) = 0;

    /**
     * Close the decoder and release all resources.
     */
    virtual void close() = 0;

    /**
     * Decode a specific frame number.
     *
     * @param frameNumber Frame number to decode (0-based)
     * @param outFrame Output frame to receive decoded data
     * @return Result::Success on success, error code otherwise
     */
    virtual Result decodeFrame(FrameNumber frameNumber, DecodedFrame& outFrame) = 0;

    /**
     * Seek to a specific frame number.
     * Positions the decoder for subsequent decode operations.
     *
     * @param frameNumber Frame number to seek to (0-based)
     * @return Result::Success on success, error code otherwise
     */
    virtual Result seek(FrameNumber frameNumber) = 0;

    /**
     * Get media type (ProRes 4444, HAP, PNG, etc.)
     */
    virtual MediaType getMediaType() const = 0;

    /**
     * Get video width in pixels.
     */
    virtual uint32_t getWidth() const = 0;

    /**
     * Get video height in pixels.
     */
    virtual uint32_t getHeight() const = 0;

    /**
     * Get frame rate (frames per second).
     */
    virtual double getFrameRate() const = 0;

    /**
     * Get total number of frames in media.
     */
    virtual FrameNumber getDuration() const = 0;

    /**
     * Check if media has an alpha channel.
     */
    virtual bool hasAlpha() const = 0;

    /**
     * Check if decoder is currently open.
     */
    virtual bool isOpen() const = 0;

    /**
     * Get file path of currently opened media.
     */
    virtual const std::string& getFilePath() const = 0;
};

/**
 * Create a decoder for the specified media type.
 *
 * Factory function that returns the appropriate decoder implementation
 * based on the media type.
 *
 * @param mediaType Type of media to decode
 * @return Unique pointer to decoder, or nullptr if type not supported
 */
std::unique_ptr<Decoder> createDecoder(MediaType mediaType);

/**
 * Detect media type from file extension.
 *
 * @param filepath Path to media file
 * @return Detected MediaType, or MediaType::Unknown if not recognized
 */
MediaType detectMediaType(const std::string& filepath);

/**
 * Probe the source codec name (FFmpeg's `avcodec_get_name` short form,
 * e.g. "hap", "prores", "h264", "hevc", "vp9", "av1"). Image sequences
 * return their format ("png", "dpx"). Returns an empty string if the
 * file can't be opened or carries no video stream.
 *
 * Used by MediaBin to color-code rows by playback tier (#27).
 */
std::string probeSourceCodecName(const std::string& filepath);

} // namespace entity
