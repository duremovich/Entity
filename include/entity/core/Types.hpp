#pragma once

#include <cstdint>
#include <string>

namespace entity {

// Media type enumeration
enum class MediaType {
    Unknown,
    VideoProRes4444,
    VideoHAP,         // Hap1 — RGB BC1
    VideoHAPAlpha,    // Hap5 — RGBA BC3
    VideoHAPQ,        // HapY — scaled YCoCg in BC3
    VideoHAPR,        // Hap7 — RGBA BC7 (newer, higher-quality HAP variant)
    PNGSequence,
    DPXSequence       // Future
};

// Blend mode enumeration (After Effects style)
enum class BlendMode {
    Normal,
    Add,
    Multiply,
    Screen,
    Overlay,
    SoftLight,
    HardLight,
    ColorDodge,
    ColorBurn,
    Darken,
    Lighten,
    Difference,
    Exclusion
};

// Transport state for playback control
enum class TransportState {
    Stopped,
    Playing,
    Paused
};

// Pixel format for decoded frames
enum class PixelFormat {
    Unknown,
    RGBA8,          // 8-bit RGBA (premultiplied alpha)
    RGBA16,         // 16-bit RGBA
    RGBA32F,        // 32-bit float RGBA
    RGB8,           // 8-bit RGB (no alpha)
    YUV420,         // YUV 4:2:0
    YUV444          // YUV 4:4:4
};

// GPU-facing texture format. Maps 1:1 onto DXGI_FORMAT at the D3D12
// boundary. Used by TextureUploader / IRenderer to create video
// textures of the right kind for the decoded content.
// RGBA8_UNORM is the default for legacy / CPU-decoded content; the
// BC* variants carry pre-compressed HAP payloads uploaded directly
// to the GPU (zero CPU decompression).
enum class TextureFormat : uint8_t {
    RGBA8_UNORM,    // Default — existing ProRes / PNG / H264 CPU-decoded paths
    BC1_UNORM,      // HAP
    BC3_UNORM,      // HAP Alpha / HAP Q (YCoCg lives here, shader decodes)
    BC4_UNORM,      // HAP Alpha-only
    BC6H_UF16,      // HAP HDR unsigned float
    BC6H_SF16,      // HAP HDR signed float
    BC7_UNORM       // HAP R
};

// Sampler-side colour-space hint for a video texture. The pixel shader uses
// this to decide whether to do an in-shader colour-space conversion before
// composition. Format alone isn't enough: HAP Alpha and HAP Q are both BC3
// but only HAP Q needs YCoCg→RGB. Numeric values must match the
// COLOR_SPACE_* defines in shaders/common.hlsli.
enum class TextureColorSpace : uint8_t {
    Linear        = 0,  // Direct RGB or RGBA — Hap, Hap Alpha, Hap R, ProRes, PNG, H.264
    YCoCg_scaled  = 1,  // Hap Q (HapY / HapM colour plane) — shader unscales + CoCg→RGB
    // Future: HDR_float (Hap HDR with FP16 compose target), Rec709, etc.
};

// Result codes for operations
enum class Result {
    Success,
    Failure,
    NotImplemented,
    InvalidParameter,
    FileNotFound,
    UnsupportedFormat,
    OutOfMemory,
    DeviceError,
    DecoderError,
    EndOfStream    // Decoder exhausted — no more frames available, not an error
};

// Common type aliases
using EntityID = uint64_t;
using FrameNumber = int64_t;
using Timestamp = int64_t;  // In microseconds
using Timecode = int64_t;   // In microseconds (timeline position)

// Constants
constexpr uint32_t INVALID_ENTITY_ID = 0;
constexpr FrameNumber INVALID_FRAME = -1;
constexpr size_t DEFAULT_FRAME_BUFFER_SIZE = 32;  // 32 frames buffered

// Helper functions
inline const char* MediaTypeToString(MediaType type) {
    switch (type) {
        case MediaType::VideoProRes4444: return "ProRes 4444";
        case MediaType::VideoHAP: return "HAP";
        case MediaType::VideoHAPAlpha: return "HAP Alpha";
        case MediaType::VideoHAPQ: return "HAP Q";
        case MediaType::VideoHAPR: return "HAP R";
        case MediaType::PNGSequence: return "PNG Sequence";
        case MediaType::DPXSequence: return "DPX Sequence";
        default: return "Unknown";
    }
}

inline const char* BlendModeToString(BlendMode mode) {
    switch (mode) {
        case BlendMode::Normal: return "Normal";
        case BlendMode::Add: return "Add";
        case BlendMode::Multiply: return "Multiply";
        case BlendMode::Screen: return "Screen";
        case BlendMode::Overlay: return "Overlay";
        case BlendMode::SoftLight: return "Soft Light";
        case BlendMode::HardLight: return "Hard Light";
        case BlendMode::ColorDodge: return "Color Dodge";
        case BlendMode::ColorBurn: return "Color Burn";
        case BlendMode::Darken: return "Darken";
        case BlendMode::Lighten: return "Lighten";
        case BlendMode::Difference: return "Difference";
        case BlendMode::Exclusion: return "Exclusion";
        default: return "Unknown";
    }
}

inline const char* TransportStateToString(TransportState state) {
    switch (state) {
        case TransportState::Stopped: return "Stopped";
        case TransportState::Playing: return "Playing";
        case TransportState::Paused: return "Paused";
        default: return "Unknown";
    }
}

inline const char* ResultToString(Result result) {
    switch (result) {
        case Result::Success: return "Success";
        case Result::Failure: return "Failure";
        case Result::NotImplemented: return "Not Implemented";
        case Result::InvalidParameter: return "Invalid Parameter";
        case Result::FileNotFound: return "File Not Found";
        case Result::UnsupportedFormat: return "Unsupported Format";
        case Result::OutOfMemory: return "Out of Memory";
        case Result::DeviceError: return "Device Error";
        case Result::DecoderError: return "Decoder Error";
        case Result::EndOfStream: return "End of Stream";
        default: return "Unknown";
    }
}

} // namespace entity
