#pragma once

#include <string>
#include <cstdint>
#include <glm/glm.hpp>

namespace entity {

enum class TextAlignment : uint8_t {
    Left   = 0,
    Center = 1,
    Right  = 2,
};

// Per-layer text state for Generative layers of the Text sub-kind.
//
// Soft-rule exception: holds std::string members. One instance per layer,
// never iterated in a per-frame hot-path view — same reasoning as
// MunchersGameState. Documented here alongside that exception.
//
// Presence on an entity (alongside GenerativeLayer) tells TextSystem and
// the compositor to treat it as a Text layer (composition-over-enum per
// ADR-0016/0018).
struct TextLayerState {
    std::string   text        = "Text";
    std::string   fontFamily  = "Segoe UI";
    float         fontSize    = 96.0f;
    glm::vec4     color       = {1.f, 1.f, 1.f, 1.f};
    TextAlignment alignment   = TextAlignment::Center;
    bool          bold        = false;
    bool          italic      = false;

    // --- Runtime fields (not persisted) ---
    bool     dirty       = true;   // set true when any authoring field changes
    int      textureSlot = -1;     // video-pool descriptor slot, -1 = not yet allocated
    uint32_t bakedWidth  = 0;
    uint32_t bakedHeight = 0;
};

} // namespace entity
