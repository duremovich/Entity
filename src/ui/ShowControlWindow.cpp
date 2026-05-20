#include "entity/ui/ShowControlWindow.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/core/Engine.hpp"
#include "entity/dmx/DmxMapping.hpp"
#include "entity/project/ProjectManager.hpp"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace entity {

namespace {

// ---------------------------------------------------------------------------
// DMX tab state

struct DmxTabState {
    std::vector<entity::dmx::Mapping> mappings;
    std::string                       lastSyncedJson;
    bool                              jsonModeOpen{false};
    std::string                       jsonBuf;
};

DmxTabState& dmxState() {
    static DmxTabState s;
    return s;
}

const char* kCommandPresets[] = {
    "Play", "Pause", "SectionGo", "FireCue", "SeekToFrame",
    "SetInputChannel", "SetDmxOut",
};

const char* kTriggerKindLabels[] = {
    "Threshold edge",       // ThresholdEdge
    "Continuous",            // Continuous
    "Cue number (slot)",     // CueNumberSlot
    "Contiguous cue block",  // ContiguousCueBlock
};

void syncFromJsonIfChanged(ProjectManager& pm, DmxTabState& s) {
    std::string current = pm.getDmxMappingsJson();
    if (current == s.lastSyncedJson) return;
    s.mappings = entity::dmx::parseMappingsJson(current);
    s.lastSyncedJson = current;
}

void writeBack(Engine& engine, ProjectManager& pm, DmxTabState& s) {
    s.lastSyncedJson = entity::dmx::serializeMappingsJson(s.mappings);
    if (auto* dispatcher = engine.getCommandDispatcher()) {
        dispatcher->enqueue(std::make_unique<SetDmxMappingsJsonCommand>(
            s.lastSyncedJson));
    } else {
        pm.setDmxMappingsJson(s.lastSyncedJson);
    }
}

bool textCell(const char* label, std::string& value, int maxLen = 256) {
    char buf[512];
    const size_t copyLen = std::min(value.size(),
                                     static_cast<size_t>(maxLen - 1));
    std::memcpy(buf, value.data(), copyLen);
    buf[copyLen] = '\0';
    ImGui::PushItemWidth(-1.0f);
    const bool changed = ImGui::InputText(label, buf, maxLen);
    ImGui::PopItemWidth();
    if (changed) value.assign(buf);
    return changed;
}

bool kindCombo(const char* label, entity::dmx::TriggerKind& kind) {
    int idx = static_cast<int>(kind);
    ImGui::PushItemWidth(-1.0f);
    const bool changed = ImGui::Combo(label, &idx,
                                       kTriggerKindLabels,
                                       IM_ARRAYSIZE(kTriggerKindLabels));
    ImGui::PopItemWidth();
    if (changed) kind = static_cast<entity::dmx::TriggerKind>(idx);
    return changed;
}

bool commandCombo(const char* label, std::string& cmd) {
    int current = -1;
    for (int i = 0; i < IM_ARRAYSIZE(kCommandPresets); ++i) {
        if (cmd == kCommandPresets[i]) { current = i; break; }
    }
    const char* preview = (current >= 0) ? kCommandPresets[current]
                                         : (cmd.empty() ? "(none)" : cmd.c_str());
    bool changed = false;
    ImGui::PushItemWidth(-1.0f);
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 0; i < IM_ARRAYSIZE(kCommandPresets); ++i) {
            const bool selected = (i == current);
            if (ImGui::Selectable(kCommandPresets[i], selected)) {
                cmd = kCommandPresets[i];
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    char buf[64];
    const size_t copyLen = std::min(cmd.size(), sizeof(buf) - 1);
    std::memcpy(buf, cmd.data(), copyLen);
    buf[copyLen] = '\0';
    ImGui::PushItemWidth(-1.0f);
    if (ImGui::InputText("##custom", buf, sizeof(buf))) {
        cmd.assign(buf);
        changed = true;
    }
    ImGui::PopItemWidth();
    return changed;
}

void renderTableEditor(Engine& engine, ProjectManager& pm, DmxTabState& s) {
    syncFromJsonIfChanged(pm, s);

    if (ImGui::Button("+ Add row")) {
        s.mappings.push_back(entity::dmx::Mapping{});
        writeBack(engine, pm, s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset to baked defaults")) {
        s.mappings.clear();
        writeBack(engine, pm, s);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Show JSON", &s.jsonModeOpen);
    ImGui::SameLine();
    ImGui::TextDisabled("(%d row%s)", static_cast<int>(s.mappings.size()),
                         s.mappings.size() == 1 ? "" : "s");

    if (s.mappings.empty()) {
        ImGui::TextWrapped(
            "No project-scoped mappings -- plugin will use baked defaults:\n"
            "  universe 0 ch 1..4 -> Play / Pause / Stop / SectionGo\n"
            "  universe 0 ch 5..8 -> FireCue 1..4");
    } else {
        constexpr ImGuiTableFlags kFlags =
            ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
        if (ImGui::BeginTable("dmxMappingsTable", 8, kFlags)) {
            ImGui::TableSetupColumn("Universe",  ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Start ch",  ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Count",     ImGuiTableColumnFlags_WidthFixed, 60);
            ImGui::TableSetupColumn("Trigger",   ImGuiTableColumnFlags_WidthFixed, 160);
            ImGui::TableSetupColumn("Command",   ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Params",    ImGuiTableColumnFlags_WidthStretch, 1.5f);
            ImGui::TableSetupColumn("Label",     ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("",          ImGuiTableColumnFlags_WidthFixed, 40);
            ImGui::TableHeadersRow();

            int rowToDelete = -1;
            for (int i = 0; i < static_cast<int>(s.mappings.size()); ++i) {
                auto& m = s.mappings[i];
                ImGui::PushID(i);
                ImGui::TableNextRow();
                bool dirty = false;

                ImGui::TableSetColumnIndex(0);
                int universe = static_cast<int>(m.universe);
                ImGui::PushItemWidth(-1.0f);
                if (ImGui::InputInt("##universe", &universe, 0)) {
                    m.universe = static_cast<std::uint16_t>(
                        std::clamp(universe, 0, 32767));
                    dirty = true;
                }
                ImGui::PopItemWidth();

                ImGui::TableSetColumnIndex(1);
                int startCh = static_cast<int>(m.startChannel);
                ImGui::PushItemWidth(-1.0f);
                if (ImGui::InputInt("##startCh", &startCh, 0)) {
                    m.startChannel = static_cast<std::uint16_t>(
                        std::clamp(startCh, 1, 512));
                    dirty = true;
                }
                ImGui::PopItemWidth();

                ImGui::TableSetColumnIndex(2);
                int count = static_cast<int>(m.channelCount);
                ImGui::PushItemWidth(-1.0f);
                if (ImGui::InputInt("##count", &count, 0)) {
                    m.channelCount = static_cast<std::uint16_t>(
                        std::clamp(count, 1, 512));
                    dirty = true;
                }
                ImGui::PopItemWidth();

                ImGui::TableSetColumnIndex(3);
                if (kindCombo("##kind", m.kind)) dirty = true;

                ImGui::TableSetColumnIndex(4);
                if (commandCombo("##cmd", m.commandType)) dirty = true;

                ImGui::TableSetColumnIndex(5);
                if (textCell("##params", m.paramsTemplate)) dirty = true;

                ImGui::TableSetColumnIndex(6);
                if (textCell("##label", m.label)) dirty = true;

                ImGui::TableSetColumnIndex(7);
                if (ImGui::SmallButton("X##rm")) rowToDelete = i;

                if (dirty) writeBack(engine, pm, s);
                ImGui::PopID();
            }
            ImGui::EndTable();

            if (rowToDelete >= 0) {
                s.mappings.erase(s.mappings.begin() + rowToDelete);
                writeBack(engine, pm, s);
            }
        }
    }

    if (s.jsonModeOpen) {
        ImGui::Separator();
        ImGui::TextDisabled("Raw JSON (read-only mirror; edit the table above)");
        s.jsonBuf = entity::dmx::serializeMappingsJson(s.mappings);
        ImGui::PushItemWidth(-1.0f);
        ImVec2 sz = ImVec2(0.0f, 120.0f);
        ImGui::InputTextMultiline("##jsonView", s.jsonBuf.data(),
                                   s.jsonBuf.size() + 1, sz,
                                   ImGuiInputTextFlags_ReadOnly);
        ImGui::PopItemWidth();
    }
}

// ---------------------------------------------------------------------------
// OSC inbound tab state

struct OscInboundRoute {
    std::string address;    // OSC address pattern, e.g. /entity/play
    std::string command;    // command type name
    std::string params;     // params template JSON fragment
};

struct OscInboundTabState {
    std::vector<OscInboundRoute> routes;
    std::string                  lastSyncedJson;
};

OscInboundTabState& oscInboundState() {
    static OscInboundTabState s;
    return s;
}

// Parse the inbound JSON bare array produced by the receiver plugin.
// Shape: [ { "address": "...", "captureKey": "...", "commands": [{"type":"...","params":"..."}] } ]
// Multi-command routes are flattened into one row per command for simple editing.
// Returns empty on any parse error (UI treats as "using defaults").
std::vector<OscInboundRoute> parseOscInboundJson(const std::string& json) {
    std::vector<OscInboundRoute> result;
    if (json.empty()) return result;
    try {
        auto arr = nlohmann::json::parse(json);
        if (!arr.is_array()) return result;
        for (const auto& route : arr) {
            std::string addr;
            if (route.contains("address") && route["address"].is_string())
                addr = route["address"].get<std::string>();
            if (!route.contains("commands") || !route["commands"].is_array())
                continue;
            for (const auto& cmd : route["commands"]) {
                std::string type, params;
                if (cmd.contains("type") && cmd["type"].is_string())
                    type = cmd["type"].get<std::string>();
                if (cmd.contains("params") && cmd["params"].is_string())
                    params = cmd["params"].get<std::string>();
                if (!type.empty())
                    result.push_back({addr, type, params});
            }
        }
    } catch (const nlohmann::json::exception&) {}
    return result;
}

// Serialize the inbound route table to the bare-array JSON the receiver parser expects.
// Consecutive rows with the same address are grouped into one route object so that
// multi-command routes (e.g. /entity/stop -> Pause + SeekToFrame) survive a round-trip.
// Note: rows that were non-consecutive for the same address get merged into one entry;
// reordering rows for the same address across other addresses can change grouping.
std::string serializeOscInboundJson(const std::vector<OscInboundRoute>& routes) {
    nlohmann::json arr = nlohmann::json::array();
    std::size_t i = 0;
    while (i < routes.size()) {
        const std::string& addr = routes[i].address;
        // Collect consecutive rows sharing this address.
        std::size_t j = i;
        while (j < routes.size() && routes[j].address == addr) ++j;

        nlohmann::json cmds = nlohmann::json::array();
        for (std::size_t k = i; k < j; ++k) {
            cmds.push_back({{"type", routes[k].command}, {"params", routes[k].params}});
        }
        arr.push_back({{"address", addr}, {"commands", cmds}});
        i = j;
    }
    return arr.dump();
}

const char* kOscCommandPresets[] = {
    "Play", "Pause", "Stop", "SectionGo", "FireCue",
    "SeekToFrame", "SetInputChannel",
};

void syncOscInboundIfChanged(ProjectManager& pm, OscInboundTabState& s) {
    std::string current = pm.getOscInboundMappingsJson();
    if (current == s.lastSyncedJson) return;
    s.routes        = parseOscInboundJson(current);
    s.lastSyncedJson = current;
}

void writeBackOscInbound(ProjectManager& pm, OscInboundTabState& s) {
    s.lastSyncedJson = serializeOscInboundJson(s.routes);
    pm.setOscInboundMappingsJson(s.lastSyncedJson);
}

bool oscCommandCombo(const char* label, std::string& cmd) {
    int current = -1;
    for (int i = 0; i < IM_ARRAYSIZE(kOscCommandPresets); ++i) {
        if (cmd == kOscCommandPresets[i]) { current = i; break; }
    }
    const char* preview = (current >= 0) ? kOscCommandPresets[current]
                                         : (cmd.empty() ? "(none)" : cmd.c_str());
    bool changed = false;
    ImGui::PushItemWidth(-1.0f);
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 0; i < IM_ARRAYSIZE(kOscCommandPresets); ++i) {
            const bool sel = (i == current);
            if (ImGui::Selectable(kOscCommandPresets[i], sel)) {
                cmd     = kOscCommandPresets[i];
                changed = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();
    return changed;
}

void renderOscInboundTab(ProjectManager& pm, OscInboundTabState& s) {
    syncOscInboundIfChanged(pm, s);

    if (ImGui::Button("+ Add route")) {
        OscInboundRoute r;
        r.address = "/entity/";
        r.command = "Play";
        s.routes.push_back(std::move(r));
        writeBackOscInbound(pm, s);
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore defaults")) {
        s.routes.clear();
        s.lastSyncedJson.clear();
        pm.setOscInboundMappingsJson("");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%d row%s)", static_cast<int>(s.routes.size()),
                         s.routes.size() == 1 ? "" : "s");

    if (s.routes.empty()) {
        ImGui::TextWrapped(
            "No project-scoped routes -- plugin uses baked defaults:\n"
            "  /entity/play   /entity/pause   /entity/stop\n"
            "  /entity/section/next   /entity/cue/{number}/go\n"
            "  /entity/seek <frame>   /entity/munch/{dir}");
        return;
    }

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("oscInboundTable", 4, kFlags)) return;

    ImGui::TableSetupColumn("Address",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Command",  ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("Params",   ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed,   40);
    ImGui::TableHeadersRow();

    int rowToDelete = -1;
    for (int i = 0; i < static_cast<int>(s.routes.size()); ++i) {
        auto& r = s.routes[i];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        bool dirty = false;

        ImGui::TableSetColumnIndex(0);
        if (textCell("##addr", r.address, 256)) dirty = true;

        ImGui::TableSetColumnIndex(1);
        if (oscCommandCombo("##cmd", r.command)) dirty = true;

        ImGui::TableSetColumnIndex(2);
        {
            // Empty params = valid (no-arg route). Non-empty must parse as JSON.
            const bool paramsValid = r.params.empty() ||
                nlohmann::json::accept(r.params);
            if (!paramsValid)
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
            if (textCell("##params", r.params, 256)) dirty = true;
            if (!paramsValid)
                ImGui::PopStyleColor();
        }

        ImGui::TableSetColumnIndex(3);
        if (ImGui::SmallButton("X##rm")) rowToDelete = i;

        if (dirty) writeBackOscInbound(pm, s);
        ImGui::PopID();
    }
    ImGui::EndTable();

    if (rowToDelete >= 0) {
        s.routes.erase(s.routes.begin() + rowToDelete);
        writeBackOscInbound(pm, s);
    }
}

// ---------------------------------------------------------------------------
// OSC outbound tab state

struct OscOutboundEvent {
    const char* id;           // event key used in the JSON
    const char* defaultAddr;  // default OSC address the plugin uses
    std::string address;      // override (empty = use default)
    bool        enabled{true};
};

struct OscOutboundTabState {
    // Fixed 9-element table mirroring the sender plugin's event set.
    OscOutboundEvent events[9] = {
        { "transport.state",        "/entity/out/transport/state",       "", true },
        { "transport.frame",        "/entity/out/transport/frame",       "", true },
        { "transport.seconds",      "/entity/out/transport/seconds",     "", true },
        { "section.active.index",   "/entity/out/section/active/index",  "", true },
        { "section.active.frame",   "/entity/out/section/active/frame",  "", true },
        { "section.next.index",     "/entity/out/section/next/index",    "", true },
        { "section.next.frame",     "/entity/out/section/next/frame",    "", true },
        { "project.name",           "/entity/out/project/name",          "", true },
        { "heartbeat",              "/entity/out/heartbeat",             "", true },
    };
    std::string lastSyncedJson;
};

OscOutboundTabState& oscOutboundState() {
    static OscOutboundTabState s;
    return s;
}

// Parse "{ "events": { "id": { "enabled": bool, "address": "..." } } }"
// Only touches the 9 known event IDs; unknown keys are silently ignored.
void parseOscOutboundJson(const std::string& json, OscOutboundTabState& s) {
    // Reset to defaults first.
    for (auto& ev : s.events) {
        ev.address = "";
        ev.enabled = true;
    }
    if (json.empty()) return;

    // Find "events" value object via simple string search.
    const char* eventsKey = "\"events\"";
    auto evPos = json.find(eventsKey);
    if (evPos == std::string::npos) return;
    evPos += std::strlen(eventsKey);
    while (evPos < json.size() && (json[evPos] == ' ' || json[evPos] == '\t' ||
                                   json[evPos] == '\n' || json[evPos] == '\r' ||
                                   json[evPos] == ':'))
        ++evPos;
    if (evPos >= json.size() || json[evPos] != '{') return;

    // Walk the events object.  Depth tracking handles nested {}.
    ++evPos; // skip opening '{'
    while (evPos < json.size() && json[evPos] != '}') {
        // Skip whitespace and commas.
        while (evPos < json.size() && (json[evPos] == ' ' || json[evPos] == '\t' ||
                                       json[evPos] == '\n' || json[evPos] == '\r' ||
                                       json[evPos] == ','))
            ++evPos;
        if (evPos >= json.size() || json[evPos] == '}') break;
        if (json[evPos] != '"') break;

        // Read event id string.
        ++evPos;
        std::size_t idStart = evPos;
        while (evPos < json.size() && json[evPos] != '"') ++evPos;
        std::string id = json.substr(idStart, evPos - idStart);
        if (evPos < json.size()) ++evPos; // closing quote

        // Skip ':'
        while (evPos < json.size() && json[evPos] != '{') ++evPos;
        if (evPos >= json.size() || json[evPos] != '{') break;
        ++evPos;

        // Read "enabled" and "address" from inner object.
        bool        enabled = true;
        std::string address;
        while (evPos < json.size() && json[evPos] != '}') {
            while (evPos < json.size() && (json[evPos] == ' ' || json[evPos] == '\t' ||
                                           json[evPos] == '\n' || json[evPos] == '\r' ||
                                           json[evPos] == ','))
                ++evPos;
            if (evPos >= json.size() || json[evPos] == '}') break;
            if (json[evPos] != '"') break;
            ++evPos;
            std::size_t kStart = evPos;
            while (evPos < json.size() && json[evPos] != '"') ++evPos;
            std::string k = json.substr(kStart, evPos - kStart);
            if (evPos < json.size()) ++evPos;

            while (evPos < json.size() && json[evPos] != ':') ++evPos;
            ++evPos;
            while (evPos < json.size() && (json[evPos] == ' ' || json[evPos] == '\t')) ++evPos;

            if (k == "enabled") {
                if (evPos + 3 < json.size() && json.substr(evPos, 4) == "true")  { enabled = true;  evPos += 4; }
                if (evPos + 4 < json.size() && json.substr(evPos, 5) == "false") { enabled = false; evPos += 5; }
            } else if (k == "address") {
                if (evPos < json.size() && json[evPos] == '"') {
                    ++evPos;
                    std::size_t aStart = evPos;
                    while (evPos < json.size() && json[evPos] != '"') ++evPos;
                    address = json.substr(aStart, evPos - aStart);
                    if (evPos < json.size()) ++evPos;
                }
            }
        }
        if (evPos < json.size() && json[evPos] == '}') ++evPos;

        // Apply to matching event slot.
        for (auto& ev : s.events) {
            if (id == ev.id) {
                ev.enabled = enabled;
                ev.address = address;
                break;
            }
        }
    }
}

std::string serializeOscOutboundJson(const OscOutboundTabState& s) {
    std::string out = "{\"events\":{";
    bool first = true;
    for (const auto& ev : s.events) {
        if (!first) out += ",";
        first = false;
        out += "\"";
        out += ev.id;
        out += "\":{\"enabled\":";
        out += ev.enabled ? "true" : "false";
        out += ",\"address\":\"";
        for (char c : ev.address) {
            if (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else           out.push_back(c);
        }
        out += "\"}";
    }
    out += "}}";
    return out;
}

void syncOscOutboundIfChanged(ProjectManager& pm, OscOutboundTabState& s) {
    std::string current = pm.getOscOutboundMappingsJson();
    if (current == s.lastSyncedJson) return;
    parseOscOutboundJson(current, s);
    s.lastSyncedJson = current;
}

void writeBackOscOutbound(ProjectManager& pm, OscOutboundTabState& s) {
    s.lastSyncedJson = serializeOscOutboundJson(s);
    pm.setOscOutboundMappingsJson(s.lastSyncedJson);
}

void renderOscOutboundTab(ProjectManager& pm, OscOutboundTabState& s) {
    syncOscOutboundIfChanged(pm, s);

    if (ImGui::Button("Restore defaults")) {
        pm.setOscOutboundMappingsJson("");
        s.lastSyncedJson.clear();
        for (auto& ev : s.events) {
            ev.address = "";
            ev.enabled = true;
        }
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Edits are picked up by the sender plugin on the next tick.");

    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
        ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
    if (!ImGui::BeginTable("oscOutboundTable", 3, kFlags)) return;

    ImGui::TableSetupColumn("Event",    ImGuiTableColumnFlags_WidthStretch, 1.5f);
    ImGui::TableSetupColumn("Address",  ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableSetupColumn("Enabled",  ImGuiTableColumnFlags_WidthFixed,   60);
    ImGui::TableHeadersRow();

    for (int i = 0; i < 9; ++i) {
        auto& ev = s.events[i];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        bool dirty = false;

        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(ev.id);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Default: %s", ev.defaultAddr);

        ImGui::TableSetColumnIndex(1);
        // Show placeholder text for the default address.
        char buf[256];
        const size_t copyLen = std::min(ev.address.size(), sizeof(buf) - 1);
        std::memcpy(buf, ev.address.data(), copyLen);
        buf[copyLen] = '\0';
        ImGui::PushItemWidth(-1.0f);
        ImGui::SetNextItemWidth(-1.0f);
        char hint[256];
        std::snprintf(hint, sizeof(hint), "(default: %s)", ev.defaultAddr);
        if (ImGui::InputTextWithHint("##addr", hint, buf, sizeof(buf))) {
            ev.address = buf;
            dirty = true;
        }
        ImGui::PopItemWidth();

        ImGui::TableSetColumnIndex(2);
        if (ImGui::Checkbox("##en", &ev.enabled)) dirty = true;

        if (dirty) writeBackOscOutbound(pm, s);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

} // namespace

ShowControlWindow::ShowControlWindow(Engine* engine)
    : m_engine(engine) {}

void ShowControlWindow::render() {
    auto* pm = m_engine ? m_engine->getProjectManager() : nullptr;
    if (!pm) {
        ImGui::TextDisabled("No active project.");
        return;
    }

    if (ImGui::BeginTabBar("ShowControlTabs")) {
        if (ImGui::BeginTabItem("DMX")) {
            ImGui::TextWrapped(
                "Per-project DMX channel mappings. Edits write to the "
                "project file (.entity v22+) and the plugin picks them up "
                "on the next incoming packet.");
            ImGui::Separator();
            renderTableEditor(*m_engine, *pm, dmxState());
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("OSC In")) {
            ImGui::TextWrapped(
                "Per-project inbound OSC routes. Each row maps an OSC address "
                "pattern to a command. Edits are picked up by the receiver "
                "plugin on the next incoming packet. Empty table = baked defaults.");
            ImGui::Separator();
            renderOscInboundTab(*pm, oscInboundState());
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("OSC Out")) {
            ImGui::TextWrapped(
                "Per-project outbound OSC event configuration. The sender "
                "plugin broadcasts these events; override the address or "
                "disable individual events here.");
            ImGui::Separator();
            renderOscOutboundTab(*pm, oscOutboundState());
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

} // namespace entity
