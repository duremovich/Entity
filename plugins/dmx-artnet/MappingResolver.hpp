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
// Returns project-scoped mappings when the active project has them;
// falls back to baked defaults otherwise. Memoizes by raw JSON
// string so the per-packet refresh is cheap when nothing changed.
class MappingResolver {
public:
    // Look up the current mapping table. Thread-safe; returns a copy
    // because ingress threads call it once per inbound packet and the
    // mappings vector is small (<100 rows in practice).
    std::vector<Mapping> current() const;

    // Re-read the project's serialized DMX mapping table via the
    // plugin context's getStringSetting accessor. Idempotent + cheap
    // when the JSON hasn't changed (early-out on string equality).
    void refreshFromProject(entity::plugin::IPluginContext* ctx);

    // Returns the unique set of universes the current mapping table
    // references. Used by sACN multicast-join management.
    std::unordered_set<std::uint16_t> activeUniverses() const;

private:
    mutable std::mutex   m_mutex;
    std::vector<Mapping> m_projectMappings; // empty -> use baked defaults
    std::string          m_lastParsedRaw;   // last JSON we parsed; cheap dirty check
};

} // namespace entity::dmx
