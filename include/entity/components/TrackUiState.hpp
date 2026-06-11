#pragma once

#include <string>

namespace entity {

// Editor-only UI state attached per track entity.
// Carries the user-visible name and shy flag (shy added at v28 so the
// schema bumps once; Phase 9 activates the shy behavior).
//
// Soft-rule exception (std::string): only ever read/written from the
// editor thread, never iterated in a hot-path view (at most one per
// track, of which at most a few dozen exist in real projects).
// Same rationale as LayerTrackUiState / MunchersGameState.
//
// When absent (legacy files or newly-created tracks before a rename):
//   name.empty() == true -> display "Track N" (position-derived).
// Persisted by ProjectSerializer at schema v28+.
struct TrackUiState {
    std::string name;
    bool        shy{false};
};

} // namespace entity
