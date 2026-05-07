#pragma once

#include "entity/core/Types.hpp"
#include <string>

namespace entity {

// Numbered timeline marker that can be fired (UI / keyboard / script) to
// seek-and-play. Uniqueness is enforced on `number`; the owning vector is
// kept sorted ascending by `number` so lookups are binary-search.
//
// Pure value type: default member initializers, no logic. Lives outside
// the ECS components/ tree because it's owned by Timeline, not a clip.
struct CueTag {
    double      number{0.0};   // unique sort key; e.g. 1.0, 1.5, 100.25
    Timecode    timestamp{0};  // microseconds along the timeline
    std::string label;         // optional user description
};

} // namespace entity
