#pragma once

#include "../core/AssetId.hpp"
#include "../core/Types.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace entity::bus {

// Channel direction. D2R = Director→Renderer, R2D = Renderer→Director.
enum class Direction { D2R, R2D };

// One active clip's contribution to a single rendered frame.
// `transformMatrix` is the pre-baked world matrix (column-major glm::mat4
// flattened). `ocioOverride` is the resolved per-clip OCIO input color space
// (empty string = use the decoder-stamped default).
struct ClipRenderState {
    std::uint64_t entity{0};
    int slot{-1};
    FrameNumber mediaFrame{0};
    std::string ocioOverride;
    std::array<float, 16> transformMatrix{
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    float opacity{1.0f};
    BlendMode blendMode{BlendMode::Normal};
    std::uint64_t targetScreen{UINT64_MAX};
};

// One frame the Renderer should warm into the FrameCache this tick.
struct WantedFrame {
    std::uint64_t entity{0};
    FrameNumber mediaFrame{0};
    int lookahead{0};
};

// Director → Renderer per-tick state snapshot. Becomes the only data path
// from Director to Renderer once subtask 8 lands.
struct RenderFrame {
    FrameNumber frameNumber{0};
    double deltaTime{0.0};
    TransportState playState{TransportState::Stopped};
    std::vector<ClipRenderState> activeClips;
    std::vector<WantedFrame> wantedFrames;
};

// Director → Renderer. Triggers the existing capture-pass pipeline; reply
// is `CaptureCompleted` keyed by `correlationId`.
//
// `hashOnly=true`  reads compose target `slot`, computes FNV-1a, returns
//                  the hex digest.
// `hashOnly=false` writes a PNG to `pngPath`. When `fullWindow=true` the
//                  source is the back buffer (entire window with UI);
//                  otherwise compose target `slot` (video output only).
struct RequestComposeCapture {
    std::uint64_t correlationId{0};
    int slot{0};
    bool hashOnly{true};
    bool fullWindow{false};
    std::string pngPath;
    std::string goldenHashPath;
};

// Renderer → Director. Resolves a parked script-result slot keyed by
// `correlationId`.
struct CaptureCompleted {
    std::uint64_t correlationId{0};
    std::string hexHash;
    bool ok{true};
    std::string errorMessage;
};

// Director → Renderer. Renderer allocates a VideoTexture descriptor slot
// and spawns the decode worker. Reply is `ResourcesProvisioned`.
//
// `assetId` is a strong-typed wrapper around the underlying string -- a
// path today; the seam where a future content-hash migration lands first.
struct ProvisionClipResources {
    std::uint64_t entity{0};
    AssetId assetId;
    MediaType mediaType{MediaType::Unknown};
    double framerate{30.0};
    FrameNumber totalMediaFrames{0};
};

// Renderer → Director. Director writes `descriptorSlot` back into the
// clip's VideoTexture component.
struct ResourcesProvisioned {
    std::uint64_t entity{0};
    int descriptorSlot{-1};
    bool ok{true};
    std::string errorMessage;
};

// Director → Renderer. Toggles a physical output's swap chain.
struct SetOutputEnabled {
    std::uint64_t entity{0};
    bool enabled{false};
};

// Director → Renderer. Mutable Settings fields the Renderer cares about.
struct ApplySettings {
    std::uint64_t frameCacheBytes{0};
    std::string ocioConfigPath;
};

// Renderer → Director. D3D12 device-removed event surfaced to the
// crash-recovery autosave path.
struct DeviceLost {
    int hresult{0};
    std::string reason;
};

// Renderer → Director. One frame failed to upload (decoder underrun, slot
// mismatch, …). Director uses this for diagnostics + drop-rate metrics.
struct FrameDropped {
    std::uint64_t entity{0};
    FrameNumber mediaFrame{0};
    std::string reason;
};

using Message = std::variant<
    RenderFrame,
    RequestComposeCapture,
    CaptureCompleted,
    ProvisionClipResources,
    ResourcesProvisioned,
    SetOutputEnabled,
    ApplySettings,
    DeviceLost,
    FrameDropped
>;

// Stable wire identifier. Mirrors the CommandDispatcher "type" string
// convention. Changing a name is a wire-format break.
const char* messageTypeName(const Message& msg) noexcept;

} // namespace entity::bus
