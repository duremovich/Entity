#pragma once

#include "../core/Types.hpp"
#include <string>

// Forward declarations for FFmpeg types (avoid including FFmpeg headers here)
struct AVFormatContext;
struct AVCodecContext;
struct AVStream;

namespace entity {

/**
 * Clip component for media source references.
 *
 * Stores information about a media clip including its file path,
 * codec context, duration, and timeline placement.
 */
struct Clip {
    std::string filepath;               // Path to media file or sequence
    MediaType mediaType{MediaType::Unknown};

    // FFmpeg context (nullptr if not loaded)
    AVFormatContext* formatContext{nullptr};
    AVCodecContext* codecContext{nullptr};
    AVStream* stream{nullptr};
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
};

} // namespace entity
