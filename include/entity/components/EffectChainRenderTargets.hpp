#pragma once

#include <cstdint>

namespace entity {

// Per-layer ping-pong compose-target slots used by CompositorSystem
// PASS 1.5 to walk the effect chain. Both slots are allocated lazily
// the first frame the layer has a non-empty effect chain, via the
// existing R2D ack pattern (mirror of GenerativeLayerRenderTargetAllocated):
// the compositor calls `createComposeTarget`, posts
// `bus::EffectChainRenderTargetAllocated`, and `Engine::drainRendererToDirector`
// writes the slot back into this component on the editor thread.
//
// -1 means "not yet allocated"; PASS 1.5 falls back to skipping
// effects for the layer until both slots exist.
struct EffectChainRenderTargets {
    std::int32_t  slotA{-1};
    std::int32_t  slotB{-1};
    std::uint32_t width{0};
    std::uint32_t height{0};
};

} // namespace entity
