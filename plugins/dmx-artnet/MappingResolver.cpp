// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "MappingResolver.hpp"

#include "entity/plugin/PluginContext.hpp"

#include <nlohmann/json.hpp>

#include <exception>

namespace entity::dmx {

namespace {

TriggerKind parseKind(const std::string& s) {
    if (s == "Continuous")         return TriggerKind::Continuous;
    if (s == "CueNumberSlot")      return TriggerKind::CueNumberSlot;
    if (s == "ContiguousCueBlock") return TriggerKind::ContiguousCueBlock;
    return TriggerKind::ThresholdEdge;
}

} // namespace

std::vector<Mapping> MappingResolver::current() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_projectMappings.empty()) {
        return m_projectMappings;
    }
    return bakedDefaultMappings();
}

void MappingResolver::refreshFromProject(entity::plugin::IPluginContext* ctx) {
    if (ctx == nullptr) return;

    // Phase 5 wires getStringSetting on the engine side. Until then
    // there's no project-scoped state — leave the resolver pinned to
    // baked defaults.
    //
    // Forward-compatibility: when the engine grows getStringSetting,
    // this method picks up the project mappings without any further
    // plugin change. The conditional is a runtime check on the
    // returned default — if the engine returns "[]" verbatim (the
    // default we pass) we know the bridge isn't wired yet OR the
    // project has no mappings; either way we fall back.
    //
    // Phase 5 will replace this with a real parse against
    // ctx->getStringSetting("dmxMappingsJson", "[]").

    std::vector<Mapping> parsed;
    // Currently a no-op; the structure below is Phase 5's full parse,
    // kept here so the diff in Phase 5 is small.
    try {
        // Placeholder: when Phase 5 lands getStringSetting on
        // IPluginContext, replace the empty literal with the real
        // call. Until then nothing actually exercises this branch.
        const std::string raw = "[]";
        auto j = nlohmann::json::parse(raw);
        if (j.is_array()) {
            for (const auto& row : j) {
                Mapping m;
                m.universe     = static_cast<std::uint16_t>(row.value("universe",     0));
                m.startChannel = static_cast<std::uint16_t>(row.value("startChannel", 1));
                m.channelCount = static_cast<std::uint16_t>(row.value("channelCount", 1));
                m.kind         = parseKind(row.value("kind", std::string("ThresholdEdge")));
                m.commandType  = row.value("commandType",  std::string{});
                m.paramsTemplate = row.value("paramsTemplate", std::string{});
                m.scaleMin     = row.value("scaleMin", 0.0);
                m.scaleMax     = row.value("scaleMax", 255.0);
                m.label        = row.value("label", std::string{});
                parsed.push_back(std::move(m));
            }
        }
    } catch (const std::exception&) {
        // Corrupt JSON -> keep baked defaults active.
        parsed.clear();
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    m_projectMappings = std::move(parsed);
}

std::unordered_set<std::uint16_t> MappingResolver::activeUniverses() const {
    std::unordered_set<std::uint16_t> out;
    const auto mappings = current();
    out.reserve(mappings.size());
    for (const auto& m : mappings) out.insert(m.universe);
    return out;
}

} // namespace entity::dmx
