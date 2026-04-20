#pragma once

/**
 * ClipDecodeState - per-clip-entity decoder, frame buffer, and decode
 * bookkeeping. Mirrors the "component = per-entity data" contract used by
 * the rest of the ECS (previously this lived as an std::unordered_map in
 * Engine, which was the wrong home — systems had to reach into Engine for
 * per-entity state instead of iterating components directly).
 *
 * RAII exception (same as Clip): owns std::unique_ptrs and is therefore
 * move-only. Copies are deleted. Components destroyed when the entity is
 * destroyed, releasing decoder + frame.
 *
 * Access pattern: DecodeSystem + Engine query via
 *   registry.view<Clip, ClipDecodeState>()
 *   registry.try_get<ClipDecodeState>(entity)
 *   registry.emplace<ClipDecodeState>(entity)
 */

#include "../core/Types.hpp"
#include <memory>

namespace entity {

// Forward declarations — keep this header lean
class Decoder;
struct DecodedFrame;

struct ClipDecodeState {
    std::unique_ptr<Decoder> decoder;         // Owned decoder (FFmpeg / PNGSeq / ...)
    std::unique_ptr<DecodedFrame> frame;      // Last decoded frame buffer
    FrameNumber lastDecodedFrame{UINT32_MAX}; // Source frame index currently in `frame`

    ClipDecodeState() = default;
    ~ClipDecodeState() = default;

    // Move-only — copies would double-free the owned decoder/frame.
    ClipDecodeState(const ClipDecodeState&) = delete;
    ClipDecodeState& operator=(const ClipDecodeState&) = delete;
    ClipDecodeState(ClipDecodeState&&) noexcept = default;
    ClipDecodeState& operator=(ClipDecodeState&&) noexcept = default;
};

} // namespace entity
