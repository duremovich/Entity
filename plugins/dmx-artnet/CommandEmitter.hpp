// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "DefaultMappings.hpp"

#include <cstdint>
#include <vector>

namespace entity::plugin { class IPluginContext; }

namespace entity::dmx {

// Walks the active mapping table against a before/after channel
// snapshot for one universe and enqueues commands for any mapping
// whose trigger fired.
//
// Caller passes the FULL 512-byte before+after arrays (zero-padded
// for short universes) plus the universe id; the emitter filters
// mappings whose universe matches.
//
// `tag` is a short string (e.g. "artnet u=0") for log breadcrumbs
// — appended to the plugin's standard "fired Play (...)" trace so
// integration tests can grep distinct sources apart.
void emitMappingFires(entity::plugin::IPluginContext* ctx,
                       const std::vector<Mapping>&    mappings,
                       std::uint16_t                  universe,
                       const std::uint8_t*            before,
                       const std::uint8_t*            after,
                       const char*                    tag);

} // namespace entity::dmx
