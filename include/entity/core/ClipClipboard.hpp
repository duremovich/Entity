#pragma once

#include "entity/components/ContentRoutingRef.hpp"
#include "entity/components/Effect.hpp"
#include "entity/components/EffectAnimatedParameters.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/timeline/Timeline.hpp"  // Timeline::DeletedClipSnapshot

#include <entt/entt.hpp>
#include <vector>

namespace entity {

/**
 * In-process clipboard snapshot for Copy/Cut/Paste of timeline clips.
 *
 * Reuses Timeline::DeletedClipSnapshot for the fields that delete-undo
 * already captures (Clip, Layer, Transform, MediaLayer, VideoTexture
 * marker, AnimatedProperties, FrameBuffer flag). Adds the components
 * delete-undo + duplicate currently miss: ContentRoutingRef and the
 * full EffectChain with each referenced Effect entity's own
 * components (Effect, EffectParameters, EffectAnimatedParameters).
 *
 * Single-slot, owned by Engine. Cleared on closeProject / loadProject
 * because the captured entt::entity asset reference for routing only
 * survives within the same project session.
 */
struct ClipClipboardSnapshot {
    // Per-archetype state captured by Timeline::snapshotClipForDelete.
    Timeline::DeletedClipSnapshot base;

    // ADR-0022 routing — separate from the legacy clip.targetScreen
    // already in `base`. asset is shared by reference; both source and
    // paste end up pointing at the same library asset entity.
    bool                          hadContentRoutingRef{false};
    ContentRoutingRef             contentRoutingRef{};

    // Per-effect node snapshot. The effect entities themselves are
    // deep-cloned on paste so mutations on the paste don't affect the
    // source. `oldEntity` is the source-side entity ID used to remap
    // EffectConnection / EffectChain::outputNode through an
    // oldToNew map during materialization.
    bool                          hadEffectChain{false};
    EffectChain                   effectChain{};
    struct EffectNodeSnapshot {
        entt::entity              oldEntity{entt::null};
        Effect                    effect{};
        bool                      hasParams{false};
        EffectParameters          params{};
        bool                      hasAnimParams{false};
        EffectAnimatedParameters  animParams{};
    };
    std::vector<EffectNodeSnapshot> effectNodes;

    // Source-track index — paste falls back to this when no clip is
    // selected at paste time. -1 if not resolved.
    int                           sourceTrackIndex{-1};

    // True when this snapshot was populated by snapshotClipForClipboard.
    bool                          valid{false};
};

}  // namespace entity
