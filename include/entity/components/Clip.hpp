#pragma once

#include "../core/Types.hpp"
#include <string>
#include <entt/entt.hpp>

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
 * Playback mode for clips that extend beyond their source media duration.
 */
enum class PlaybackMode {
    Freeze,     // Hold on last frame
    Loop,       // Restart from beginning
    PingPong    // Play forward then backward (palindrome)
};

/**
 * Per-clip behavior policy when the timeline pauses at a section break.
 * Phase B serializes this field and exposes it through the UI but treats
 * every clip as Locked (frozen with the playhead). Phase C activates the
 * Normal continuation path so Loop/PingPong clips keep cycling past the
 * break while the playhead is parked.
 *
 * Note: this is a per-clip behavior policy, NOT a section-membership
 * reference. A clip's relationship to sections is always positional.
 */
enum class SectionBehavior : uint8_t {
    Normal,
    Locked
};

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
    FrameNumber duration{0};            // Duration in frames (on timeline)
    FrameNumber mediaStartFrame{0};     // Offset into source media
    FrameNumber totalMediaFrames{0};    // Total frames in source media
    double framerate{30.0};             // Frames per second
    PlaybackMode playbackMode{PlaybackMode::Freeze};  // Behavior when clip extends beyond source
    SectionBehavior sectionBehavior{SectionBehavior::Normal};  // Inert in Phase B (every clip treated as Locked); activated by Phase C.

    // Media properties
    uint32_t width{0};
    uint32_t height{0};
    bool hasAlpha{false};
    bool frameBlending{false};        // Enable frame blending for smoother playback at mismatched rates

    // Screen mapping
    entt::entity targetScreen{entt::null};  // Target screen for this clip (null = default/all screens)

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
        , totalMediaFrames(other.totalMediaFrames)
        , framerate(other.framerate)
        , playbackMode(other.playbackMode)
        , sectionBehavior(other.sectionBehavior)
        , width(other.width)
        , height(other.height)
        , hasAlpha(other.hasAlpha)
        , frameBlending(other.frameBlending)
        , targetScreen(other.targetScreen)
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
            totalMediaFrames = other.totalMediaFrames;
            framerate = other.framerate;
            playbackMode = other.playbackMode;
            sectionBehavior = other.sectionBehavior;
            width = other.width;
            height = other.height;
            hasAlpha = other.hasAlpha;
            frameBlending = other.frameBlending;
            targetScreen = other.targetScreen;
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
