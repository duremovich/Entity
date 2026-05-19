// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "entity/dmx/DmxMapping.hpp"

namespace entity::dmx {

// Baked default mappings used iff the active project has no
// dmxMappings array. Out-of-box experience: an operator enables the
// plugin and sends Universe 0 Channel 1 high -> Play fires without
// first learning the mapping editor.
const std::vector<Mapping>& bakedDefaultMappings();

} // namespace entity::dmx
