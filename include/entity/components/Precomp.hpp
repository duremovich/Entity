#pragma once

#include "entity/components/Clip.hpp"   // PlaybackMode
#include "entity/core/Types.hpp"

#include <entt/entt.hpp>

#include <cstdint>
#include <string>

namespace entity {

/**
 * PrecompMember — tag on a precomp definition's authored "master" layer
 * entities (ADR-0029 Decision 2). Masters live on the definition
 * timeline's tracks and are excluded from every playback system per the
 * normative exclusion table in ADR-0029 Decision 4 (enforced from Phase B
 * onward).
 */
struct PrecompMember {
    std::string definitionId;
};

/**
 * PrecompShadow — tag on a per-instance hidden clone of a definition's
 * master clip (ADR-0029 Decisions 1/4). Shadows are trackless
 * registry-only entities: ordinary Clip entities flowing through
 * decode/upload/present/cache unchanged, with only their frame input
 * substituted via mapOuterToInnerFrame. A shadow must never acquire
 * ClipPlaybackPhase or AudioSource — that is a programming error.
 * The materializer lands in Phase C.
 */
struct PrecompShadow {
    entt::entity  instance{entt::null};
    entt::entity  masterEntity{entt::null};
    std::uint32_t innerZOrder{0};
};

/**
 * PrecompInstance — component on a root-timeline precomp layer entity
 * (ADR-0029 Decision 5). The instance renders as a Compose-sourced
 * content layer filled by the PASS 1.6 producer (Phase D).
 * renderTargetSlot follows the GenerativeLayer convention: -1 until the
 * R2D allocation ack lands. materializedVersion mirrors the definition's
 * version; a mismatch triggers shadow resync (Phase E).
 */
struct PrecompInstance {
    std::string   definitionId;
    FrameNumber   innerStartFrame{0};
    double        speed{1.0};
    PlaybackMode  playbackMode{PlaybackMode::Freeze};
    std::uint64_t materializedVersion{0};
    int           renderTargetSlot{-1};
    std::uint64_t renderTargetGeneration{0};
    std::uint32_t canvasWidth{1920};
    std::uint32_t canvasHeight{1080};
};

} // namespace entity
