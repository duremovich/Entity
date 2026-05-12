#pragma once

#include "EffectParam.hpp"

#include <vector>

namespace entity {

// Per-effect parameter values. The vector is positional — index N maps
// to the Nth ParamSchema entry in the effect kind's schema. Wire
// serialization translates index → schema.name so renaming or
// reordering params in a kind doesn't silently shift values in old
// project files. The runtime index is rebuilt on load.
struct EffectParameters {
    std::vector<ParamValue> values;
};

} // namespace entity
