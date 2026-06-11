#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
//
// Single write/read path for layer placement (startFrame / duration) shared
// by commands and (eventually) UI code that needs to move layers without going
// through the full command stack.
//
// These are the ONLY functions that should write Layer placement. They are
// defined inline in this header so unit tests can call them directly and
// verify the shipped code (including the Clip->Layer sync via
// Timeline::syncLayerFromClip) without a full Engine / D3D12 / GLFW stack.
//
// Note: the clamp-reject guards in SetLayer*Command::execute() (start >= 0,
// duration >= 1) live in Commands.cpp because they require the surrounding
// command context. They are covered by the UI AlwaysClamp flag on the
// DragInt widgets and by the script-path behavior — not by unit tests that
// call execute() directly (no Engine-free seam exists for that path).
//
// Callers:
//   - SetLayerStartFrameCommand::execute / undo  (src/command/Commands.cpp)
//   - SetLayerDurationCommand::execute / undo    (src/command/Commands.cpp)
//   - Tests                                      (tests/unit/SetLayerCommandTests.cpp)

#include "entity/components/Clip.hpp"
#include "entity/components/Layer.hpp"
#include "entity/core/Types.hpp"
#include "entity/timeline/Timeline.hpp"
#include <entt/entt.hpp>

namespace entity::command {

// Write a new startFrame to a timeline-resident entity.
//
// For Clip-backed entities: writes Clip::startFrame and calls
// Timeline::syncLayerFromClip so the Layer mirror stays in sync.
// For Layer-only entities (OA / Generative / Signal): writes Layer::startFrame
// directly.
// Returns false if the entity has neither a Clip nor a Layer component.
inline bool writeLayerStartFrame(entt::registry& registry,
                                 entt::entity    e,
                                 FrameNumber     f) {
    if (auto* clip = registry.try_get<Clip>(e)) {
        clip->startFrame = f;
        Timeline::syncLayerFromClip(registry, e);
        return true;
    }
    if (auto* lay = registry.try_get<Layer>(e)) {
        lay->startFrame = f;
        return true;
    }
    return false;
}

// Write a new duration to a timeline-resident entity.
//
// For Clip-backed entities: writes Clip::duration and calls
// Timeline::syncLayerFromClip.
// For Layer-only entities: writes Layer::duration directly.
// Returns false if the entity has neither a Clip nor a Layer component.
inline bool writeLayerDuration(entt::registry& registry,
                               entt::entity    e,
                               FrameNumber     d) {
    if (auto* clip = registry.try_get<Clip>(e)) {
        clip->duration = d;
        Timeline::syncLayerFromClip(registry, e);
        return true;
    }
    if (auto* lay = registry.try_get<Layer>(e)) {
        lay->duration = d;
        return true;
    }
    return false;
}

// Read startFrame from a timeline-resident entity.
// Returns -1 if the entity has neither component.
inline FrameNumber readLayerStartFrame(const entt::registry& registry,
                                       entt::entity          e) {
    if (const auto* clip = registry.try_get<Clip>(e)) return clip->startFrame;
    if (const auto* lay  = registry.try_get<Layer>(e)) return lay->startFrame;
    return -1;
}

// Read duration from a timeline-resident entity.
// Returns -1 if the entity has neither component.
inline FrameNumber readLayerDuration(const entt::registry& registry,
                                     entt::entity          e) {
    if (const auto* clip = registry.try_get<Clip>(e)) return clip->duration;
    if (const auto* lay  = registry.try_get<Layer>(e)) return lay->duration;
    return -1;
}

} // namespace entity::command
