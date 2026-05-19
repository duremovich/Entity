// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "MappingResolver.hpp"

#include "entity/plugin/PluginContext.hpp"

namespace entity::dmx {

std::vector<Mapping> MappingResolver::current() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_projectMappings.empty()) {
        return m_projectMappings;
    }
    return bakedDefaultMappings();
}

void MappingResolver::refreshFromProject(entity::plugin::IPluginContext* ctx) {
    if (ctx == nullptr) return;

    // Read the active project's serialized DMX mapping table through
    // the IPluginContext bridge. The engine's EnginePluginContext
    // routes the "dmxMappingsJson" key to ProjectManager. Empty or
    // missing -> we leave m_projectMappings empty and current() falls
    // back to bakedDefaultMappings().
    const std::string raw = ctx->getStringSetting("dmxMappingsJson", "");

    // Cheap early-out -- avoids reparsing the same JSON every packet
    // when the project hasn't changed.
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (raw == m_lastParsedRaw) return;
    }

    std::vector<Mapping> parsed = parseMappingsJson(raw);

    // Per-change log: only fires when raw differs from cached.
    // Cheap diagnostic -- operators reloading a project want to know
    // the plugin saw their mapping change land.
    char dbg[160];
    std::snprintf(dbg, sizeof(dbg),
                   "dmx-artnet: project mappings updated (%zu rows from %zu JSON bytes)",
                   parsed.size(), raw.size());
    ctx->log(entity::plugin::LogLevel::Info, dbg);

    std::lock_guard<std::mutex> lk(m_mutex);
    m_projectMappings = std::move(parsed);
    m_lastParsedRaw   = raw;
}

std::unordered_set<std::uint16_t> MappingResolver::activeUniverses() const {
    std::unordered_set<std::uint16_t> out;
    const auto mappings = current();
    out.reserve(mappings.size());
    for (const auto& m : mappings) out.insert(m.universe);
    return out;
}

} // namespace entity::dmx
