#include "entity/ui/ShowControlWindow.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/core/Engine.hpp"
#include "entity/dmx/DmxMapping.hpp"
#include "entity/project/ProjectManager.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace entity {

namespace {

// Per-window in-memory mirror of the parsed mapping table. We re-parse
// from ProjectManager's stored JSON whenever it changes underneath us
// (project loaded / reverted), and re-serialize whenever the user
// edits a field. Storing the typed vector here keeps the per-frame
// path free of JSON parsing.
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
    const std::string& current = pm.dmxMappingsJson();
    if (current == s.lastSyncedJson) return;
    s.mappings = entity::dmx::parseMappingsJson(current);
    s.lastSyncedJson = current;
}

// Push the typed table back to the project as an undoable command.
// Each call adds one Ctrl+Z step. Coalescing per-keystroke would
// require focus tracking we don't have today; per-edit undo matches
// the rest of the editor's property-inspector behavior.
void writeBack(Engine& engine, ProjectManager& pm, DmxTabState& s) {
    s.lastSyncedJson = entity::dmx::serializeMappingsJson(s.mappings);
    if (auto* dispatcher = engine.getCommandDispatcher()) {
        dispatcher->enqueue(std::make_unique<SetDmxMappingsJsonCommand>(
            s.lastSyncedJson));
    } else {
        // Fallback: no dispatcher (shouldn't happen in normal runtime,
        // but keep the editor usable in test harnesses).
        pm.setDmxMappingsJson(s.lastSyncedJson);
    }
}

// Render a single editable string cell into a fixed buffer.
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
    // Fall-through edit: allow free-form name as well (custom commands).
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
        // Always re-render from the typed mappings so the JSON view
        // tracks edits live.
        s.jsonBuf = entity::dmx::serializeMappingsJson(s.mappings);
        ImGui::PushItemWidth(-1.0f);
        ImVec2 sz = ImVec2(0.0f, 120.0f);
        ImGui::InputTextMultiline("##jsonView", s.jsonBuf.data(),
                                   s.jsonBuf.size() + 1, sz,
                                   ImGuiInputTextFlags_ReadOnly);
        ImGui::PopItemWidth();
    }
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
        ImGui::EndTabBar();
    }
}

} // namespace entity
