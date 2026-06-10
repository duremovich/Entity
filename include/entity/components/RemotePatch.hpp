#pragma once

#include <cstdint>
#include <string>

namespace entity {

/**
 * RemotePatch — authored remote-control patch config (ADR-0028).
 *
 * Presence = this content layer is patched for remote control.
 * patchId is the address handle (/entity/layer/<patchId>/...),
 * unique per project, charset [a-z0-9_].
 *
 * storeSlot / lastTextGen are runtime-only (rebound after load).
 * Written only on the editor thread (ADR-0014). Soft-rule exception:
 * holds std::string; not iterated in any hot-path view.
 */
struct RemotePatch {
    std::string   patchId;
    bool          armedByDefault{false};
    int           storeSlot{-1};        // runtime-only
    std::uint32_t lastTextGen{0};       // runtime-only (TextSystem consume)
};

} // namespace entity
