#pragma once

#include <string>
#include <vector>

namespace entity {

// Editor-only UI state attached per layer (clip / OA / generative).
// Tracks which property-group rows the user has collapsed in the
// timeline's expanded twirl-down view. Never reaches the show thread
// or the bus — purely UX scaffolding.
//
// Soft-rule exception (vector<string>): only ever read/written from
// the editor thread, never iterated in a hot-path view (at most one
// per layer, of which at most a few dozen exist in real projects).
// Same rationale as MunchersGameState / TextLayerState / Clip's RAII
// exception — see include/entity/components/CLAUDE.md.
//
// Storage canonicalization: group paths are slash-joined strings
// matching what TimelineWidget builds in propertyListForEntity:
//   "Transform"
//   "Effects"
//   "Effects/<kind displayName>#<effect entity id>"
// Presence in `collapsedGroups` means COLLAPSED. Default-constructed
// state (no entries) means all groups expanded. Persisted by
// ProjectSerializer at schema v25+.
struct LayerTrackUiState {
    std::vector<std::string> collapsedGroups;
};

} // namespace entity
