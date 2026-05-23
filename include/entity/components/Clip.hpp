#pragma once

#include "../core/Types.hpp"
#include <string>
#include <algorithm>
#include <cmath>
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
    FrameNumber mediaStartFrame{0};     // Offset into source media (in-point, source frames, inclusive)
    FrameNumber mediaOutFrame{-1};      // Out-point: last source frame played, inclusive (NLE / video-editing convention). Sentinel -1 means "end of source" (resolved on decoder open / project load).
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
        , mediaOutFrame(other.mediaOutFrame)
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
            mediaOutFrame = other.mediaOutFrame;
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

// Source-frame span actually served by the decoder for this clip.
// Used by the wrap math (Freeze/Loop/PingPong) as the boundary at which
// playback hits the out-point. mediaOutFrame is INCLUSIVE (the last frame
// played), so the window [mediaStartFrame, mediaOutFrame] is
// (mediaOutFrame - mediaStartFrame + 1) frames long. Returns at least 1
// to keep cycle math safe.
inline FrameNumber effectivePlaybackLength(const Clip& clip) {
    if (clip.mediaOutFrame >= clip.mediaStartFrame) {
        return clip.mediaOutFrame - clip.mediaStartFrame + 1;
    }
    if (clip.totalMediaFrames > clip.mediaStartFrame) {
        return clip.totalMediaFrames - clip.mediaStartFrame;
    }
    return 1;
}

// Normal-mode break-aligned active-window extension (ADR-0012 amendment
// 2026-05-23 and the 2026-05-23 follow-up). Returns `clip.duration`
// when no extension applies. Pure math — callers supply
// `endAlignsWithBreak` and `endingBreakFadeSeconds` from Timeline
// queries (`timeline::sectionFadeTailFrames > 0` and
// `timeline::sectionFadeSecondsAtBreak` respectively). The show-side
// caller reads them from the baked ClipCatalogEntry; the editor-side
// caller queries the live Timeline. Lives here (not on
// PlaybackTimeAuthority) so DecodeSystem and AudioSystem can use the
// same math without depending on PTA.
//
// Two extension shapes, gated by the ending break's fadeSeconds:
//
//   fadeSeconds == 0  → extend all the way to source-out (Fix 8's
//                       original "play through the trim" semantic;
//                       see normal_mode_extends_past_break.json).
//
//   fadeSeconds  > 0  → extend through the fade window only
//                       (clip.duration + ceil(fadeSeconds * fps)
//                       timeline frames). Source content advances
//                       during the visible fade; clip becomes inactive
//                       at the fade end — decoder stops, cache
//                       pressure goes away. Cap at sourceTimelineFrames
//                       so a short clip never extends past its own
//                       source out point.
//
// The fadeSeconds > 0 cap fixes the two-clip-meeting-at-fade-break
// cache thrash (operator-reported 2026-05-23): pre-fix the extension
// ran past visibility, two 4K H.264 decoders competed for cache
// indefinitely, Fix 4 cache-miss recovery thrashed. Post-fix the
// extra decoder runs only for the legitimate fade window; combined
// with the 2 GiB FrameCache default, 2× 4K H.264 fits comfortably.
inline FrameNumber computeExtendedDuration(const Clip& clip,
                                            double timelineFrameRate,
                                            bool endAlignsWithBreak,
                                            double endingBreakFadeSeconds) {
    if (clip.sectionBehavior != SectionBehavior::Normal) return clip.duration;
    if (!endAlignsWithBreak) return clip.duration;
    if (timelineFrameRate <= 0.0 || clip.framerate <= 0.0) return clip.duration;
    const FrameNumber sourceFrames = effectivePlaybackLength(clip);
    if (sourceFrames <= 0) return clip.duration;
    const double frameRateRatio = clip.framerate / timelineFrameRate;
    if (frameRateRatio <= 0.0) return clip.duration;
    const FrameNumber sourceTimelineFrames = static_cast<FrameNumber>(
        std::ceil(static_cast<double>(sourceFrames) / frameRateRatio));

    if (endingBreakFadeSeconds > 0.0) {
        const FrameNumber fadeFrames = static_cast<FrameNumber>(
            std::ceil(endingBreakFadeSeconds * timelineFrameRate));
        const FrameNumber extendedToFadeEnd = clip.duration + fadeFrames;
        return std::min(std::max(clip.duration, extendedToFadeEnd),
                        sourceTimelineFrames);
    }
    return std::max(clip.duration, sourceTimelineFrames);
}

} // namespace entity
