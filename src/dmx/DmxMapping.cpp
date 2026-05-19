// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "entity/dmx/DmxMapping.hpp"

#include <nlohmann/json.hpp>

#include <exception>

namespace entity::dmx {

const char* kindToString(TriggerKind k) {
    switch (k) {
        case TriggerKind::Continuous:         return "Continuous";
        case TriggerKind::CueNumberSlot:      return "CueNumberSlot";
        case TriggerKind::ContiguousCueBlock: return "ContiguousCueBlock";
        case TriggerKind::ThresholdEdge:      // fallthrough
        default:                              return "ThresholdEdge";
    }
}

TriggerKind parseKind(const std::string& s) {
    if (s == "Continuous")         return TriggerKind::Continuous;
    if (s == "CueNumberSlot")      return TriggerKind::CueNumberSlot;
    if (s == "ContiguousCueBlock") return TriggerKind::ContiguousCueBlock;
    return TriggerKind::ThresholdEdge;
}

std::vector<Mapping> parseMappingsJson(std::string_view raw) {
    std::vector<Mapping> out;
    if (raw.empty()) return out;
    try {
        auto j = nlohmann::json::parse(raw);
        if (!j.is_array()) return out;
        out.reserve(j.size());
        for (const auto& row : j) {
            Mapping m;
            m.universe       = static_cast<std::uint16_t>(row.value("universe",     0));
            m.startChannel   = static_cast<std::uint16_t>(row.value("startChannel", 1));
            m.channelCount   = static_cast<std::uint16_t>(row.value("channelCount", 1));
            m.kind           = parseKind(row.value("kind", std::string("ThresholdEdge")));
            m.commandType    = row.value("commandType",    std::string{});
            m.paramsTemplate = row.value("paramsTemplate", std::string{});
            m.scaleMin       = row.value("scaleMin", 0.0);
            m.scaleMax       = row.value("scaleMax", 255.0);
            m.label          = row.value("label",    std::string{});
            out.push_back(std::move(m));
        }
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

std::string serializeMappingsJson(const std::vector<Mapping>& mappings) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : mappings) {
        nlohmann::json row;
        row["universe"]       = m.universe;
        row["startChannel"]   = m.startChannel;
        row["channelCount"]   = m.channelCount;
        row["kind"]           = kindToString(m.kind);
        row["commandType"]    = m.commandType;
        if (!m.paramsTemplate.empty()) row["paramsTemplate"] = m.paramsTemplate;
        if (m.scaleMin != 0.0)   row["scaleMin"] = m.scaleMin;
        if (m.scaleMax != 255.0) row["scaleMax"] = m.scaleMax;
        if (!m.label.empty()) row["label"] = m.label;
        arr.push_back(std::move(row));
    }
    return arr.dump();
}

} // namespace entity::dmx
