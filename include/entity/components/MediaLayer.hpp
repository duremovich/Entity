#pragma once

#include "../core/Types.hpp"
#include <cstdint>

namespace entity {

/**
 * MediaLayer component for layer-specific rendering properties.
 *
 * Controls z-ordering, opacity, blend modes, and visibility.
 */
struct MediaLayer {
    uint32_t zOrder{0};         // Rendering order (0 = back, higher = front)
    float opacity{1.0f};        // Layer opacity (0.0 = transparent, 1.0 = opaque)
    BlendMode blendMode{BlendMode::Normal};
    bool visible{true};         // Visibility toggle

    /**
     * Check if this layer should be rendered.
     * Invisible or fully transparent layers are skipped.
     */
    bool shouldRender() const {
        return visible && opacity > 0.0f;
    }

    /**
     * Get effective opacity (clamped between 0.0 and 1.0).
     */
    float getOpacity() const {
        if (opacity < 0.0f) return 0.0f;
        if (opacity > 1.0f) return 1.0f;
        return opacity;
    }
};

} // namespace entity
