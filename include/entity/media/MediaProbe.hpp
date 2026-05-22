#pragma once

#include "entity/core/Types.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace entity {

// Codec playback tier — drives the color-coded codec column in MediaBin
// and any UI that wants to telegraph "is this codec realtime-friendly?".
// Lives under media/ (not ui/) so the off-thread MediaProbeWorker can
// classify codecs without pulling UI headers.
enum class CodecTier {
    Unknown,
    Good,  // HAP*  — designed for realtime media servers
    OK,    // ProRes / DNxHD / image sequences — intra-frame, predictable
    Bad,   // H.264 / H.265 / VP9 / AV1 — interframe, seek-hostile
};

// Probe result. Populated by MediaProbeWorker on a background thread,
// consumed by MediaBinWindow on the render thread. Plain data, copyable.
struct ProbeInfo {
    bool          valid{false};
    bool          isHap{false};
    std::uint32_t width{0};
    std::uint32_t height{0};
    double        framerate{0.0};
    FrameNumber   totalFrames{0};
    bool          hasAlpha{false};
    bool          hasAudio{false};
    int           audioSampleRate{0};
    int           audioChannels{0};
    std::string   sourceCodecName;   // ffmpeg short form: "hap", "prores", "h264", ...
    std::string   displayCodecName;  // pretty form: "HAP", "ProRes", "H.264"
    CodecTier     tier{CodecTier::Unknown};
};

// Classify an FFmpeg codec short name into a playback tier. Free function
// so the worker thread can call it without a UI dependency.
CodecTier classifyCodec(std::string_view ffmpegName);

// Pretty-print an FFmpeg codec name for the user-facing cell.
std::string prettyCodec(std::string_view ffmpegName);

}  // namespace entity
