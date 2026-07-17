#pragma once

#include <array>

namespace entity {

// Solid generative sub-kind: a flat color the layer's compose target is
// filled with each frame. The classic hosting surface for generator /
// effect chains (blur a solid = nothing; fractal-noise a solid =
// procedural texture), and a plain color layer on its own.
//
// Presence (alongside GenerativeLayer) makes the bake treat the layer as
// a Solid — composition-over-enum per ADR-0016/0018. Pure POD, 16 bytes.
//
// Color is linear-light RGBA (same convention as compose targets); the
// alpha channel participates in PASS 2 blending like any layer texture.
struct SolidLayerState {
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
};

} // namespace entity
