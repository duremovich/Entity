#pragma once

#include "../core/Types.hpp"
#include <string>

// Forward declare FFmpeg cleanup functions
extern "C" {
    void avcodec_free_context(struct AVCodecContext** avctx);
    void avformat_close_input(struct AVFormatContext** s);
}

// Forward declarations for FFmpeg types
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;

namespace entity {

/**
 * Clip component for media source references.
 *
 * Stores information about a media clip including its file path,
 * codec context, duration, and timeline placement.
 *
 * RAII: Properly cleans up FFmpeg resources on destruction.
 * Move-only: Cannot be copied to prevent double-free of FFmpeg contexts.
 */
struct Clip {
    std::string filepath;               // Path to media file or sequence
    MediaType mediaType{MediaType::Unknown};

    // FFmpeg context (nullptr if not loaded)
    AVFormatContext* formatContext{nullptr};
    AVCodecContext* codecContext{nullptr};
    AVStream* stream{nullptr};           // Non-owning pointer (owned by formatContext)
    int streamIndex{-1};

    // Timing information
    FrameNumber startFrame{0};          // Start frame on timeline
    FrameNumber duration{0};            // Duration in frames
    FrameNumber mediaStartFrame{0};     // Offset into source media
    double framerate{30.0};             // Frames per second

    // Media properties
    uint32_t width{0};
    uint32_t height{0};
    bool hasAlpha{false};

    // State
    bool loaded{false};                 // True if media is loaded
    bool decoding{false};               // True if decode thread is running

    // RAII: Cleanup FFmpeg resources
    ~Clip() {
        if (codecContext) {
            avcodec_free_context(&codecContext);
        }
        if (formatContext) {
            avformat_close_input(&formatContext);
        }
    }

    // Move constructor: Transfer ownership
    Clip(Clip&& other) noexcept
        : filepath(std::move(other.filepath))
        , mediaType(other.mediaType)
        , formatContext(other.formatContext)
        , codecContext(other.codecContext)
        , stream(other.stream)
        , streamIndex(other.streamIndex)
        , startFrame(other.startFrame)
        , duration(other.duration)
        , mediaStartFrame(other.mediaStartFrame)
        , framerate(other.framerate)
        , width(other.width)
        , height(other.height)
        , hasAlpha(other.hasAlpha)
        , loaded(other.loaded)
        , decoding(other.decoding)
    {
        // Clear source pointers to prevent double-free
        other.formatContext = nullptr;
        other.codecContext = nullptr;
        other.stream = nullptr;
    }

    // Move assignment: Transfer ownership
    Clip& operator=(Clip&& other) noexcept {
        if (this != &other) {
            // Clean up existing resources
            if (codecContext) {
                avcodec_free_context(&codecContext);
            }
            if (formatContext) {
                avformat_close_input(&formatContext);
            }

            // Transfer ownership
            filepath = std::move(other.filepath);
            mediaType = other.mediaType;
            formatContext = other.formatContext;
            codecContext = other.codecContext;
            stream = other.stream;
            streamIndex = other.streamIndex;
            startFrame = other.startFrame;
            duration = other.duration;
            mediaStartFrame = other.mediaStartFrame;
            framerate = other.framerate;
            width = other.width;
            height = other.height;
            hasAlpha = other.hasAlpha;
            loaded = other.loaded;
            decoding = other.decoding;

            // Clear source pointers
            other.formatContext = nullptr;
            other.codecContext = nullptr;
            other.stream = nullptr;
        }
        return *this;
    }

    // Delete copy operations to prevent accidental double-free
    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;

    // Default constructor
    Clip() = default;
};

} // namespace entity
