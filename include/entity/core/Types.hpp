#pragma once

#include <cstdint>
#include <string>

namespace entity {

// Media type enumeration
enum class MediaType {
    Unknown,
    VideoProRes4444,
    VideoHAP,
    VideoHAPAlpha,
    VideoHAPQ,
    PNGSequence,
    DPXSequence  // Future
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
    DecoderError
};

// Common type aliases
using EntityID = uint64_t;
using FrameNumber = int64_t;
using Timestamp = int64_t;  // In microseconds
using Timecode = int64_t;   // In milliseconds (timeline position)

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
        default: return "Unknown";
    }
}

} // namespace entity
