// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "DefaultMappings.hpp"

#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace entity::plugin { class IPluginContext; }

namespace entity::dmx {

// Resolves the active mapping table.
//
// Phase 1: returns baked defaults always.
// Phase 5: refreshes from the active project's dmxMappingsJson via
//          IPluginContext::getStringSetting; falls back to baked
//          defaults when the project carries no mappings.
class MappingResolver {
public:
    // Look up the current mapping table. Thread-safe; returns a copy
    // because ingress threads call it once per inbound packet and the
    // mappings vector is small (<100 rows in practice).
    std::vector<Mapping> current() const;

    // Phase 5 hook: re-parse the project-scoped mappings JSON. No-op
    // in Phase 1.
    void refreshFromProject(entity::plugin::IPluginContext* ctx);

    // Returns the unique set of universes the current mapping table
    // references. Used by sACN multicast-join management.
    std::unordered_set<std::uint16_t> activeUniverses() const;

private:
    mutable std::mutex   m_mutex;
    std::vector<Mapping> m_projectMappings; // empty -> use baked defaults
};

} // namespace entity::dmx
