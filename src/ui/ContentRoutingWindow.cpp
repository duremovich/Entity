#include "entity/ui/ContentRoutingWindow.hpp"

#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/ContentRouting.hpp"
#include "entity/components/GenerativeLayer.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/timeline/Timeline.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace entity {

namespace {

// Map a clip entity back to (trackIndex, clipIndex). Mirrors PropertyWindow's
// helper — kept local to avoid pulling that whole window's header in.
std::optional<std::pair<int, int>>
findClipIndices(Timeline* timeline, entt::entity entity) {
    if (!timeline || entity == entt::null) return std::nullopt;
    auto& registry = timeline->getRegistry();
    const auto& tracks = timeline->getTracks();
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        auto* track = registry.try_get<TimelineTrack>(tracks[ti]);
        if (!track) continue;
        for (size_t ci = 0; ci < track->layers.size(); ++ci) {
            if (track->layers[ci] == entity) {
                return std::make_pair(static_cast<int>(ti), static_cast<int>(ci));
            }
        }
    }
    return std::nullopt;
}

// Snapshot a ContentRouting into the wire-shape SetContentRoutingCommand
// uses (screen NAMES instead of entt::entity IDs so the command round-trips
// through project saves and undo across reloads).
struct DraftRouting {
    std::string mode{"Direct"};
    std::vector<SetContentRoutingCommand::TargetSpec> targets;
};

DraftRouting captureDraft(entt::registry& registry, entt::entity entity) {
    DraftRouting d;
    const auto* cr = registry.try_get<ContentRouting>(entity);
    if (!cr) return d;
    d.mode = (cr->mode == RouteMode::Tiled) ? "Tiled" : "Direct";
    for (const auto& t : cr->targets) {
        SetContentRoutingCommand::TargetSpec spec;
        spec.uvRect = t.uvRect;
        if (t.screen != entt::null &&
            registry.valid(t.screen) &&
            registry.all_of<Screen>(t.screen)) {
            spec.screenName = registry.get<Screen>(t.screen).name;
        }
        d.targets.push_back(std::move(spec));
    }
    return d;
}

void dispatchDraft(CommandDispatcher* dispatcher, Timeline* timeline,
                    entt::entity entity, const DraftRouting& d) {
    if (!dispatcher || !timeline) return;
    auto idx = findClipIndices(timeline, entity);
    if (!idx) return;
    auto cmd = std::make_unique<SetContentRoutingCommand>(
        idx->first, idx->second, d.mode, d.targets);
    dispatcher->enqueue(std::move(cmd));
}

} // namespace

ContentRoutingWindow::ContentRoutingWindow(Timeline* timeline)
    : m_timeline(timeline) {}

void ContentRoutingWindow::render() {
    if (!m_timeline) {
        ImGui::TextDisabled("No timeline.");
        return;
    }

    auto& registry = m_timeline->getRegistry();
    const entt::entity selected = m_timeline->getSelectedClip();
    if (selected == entt::null || !registry.valid(selected)) {
        ImGui::TextDisabled("Select a clip or generative layer in the timeline");
        ImGui::TextDisabled("to edit its content routing.");
        return;
    }

    // Only clip-backed and generative-backed layers carry ContentRouting in
    // M3. Object-animation layers don't produce content texture, so routing
    // doesn't apply to them.
    const bool isClip       = registry.all_of<Clip>(selected);
    const bool isGenerative = registry.all_of<GenerativeLayer>(selected);
    if (!isClip && !isGenerative) {
        ImGui::TextDisabled("Selected layer doesn't produce content");
        ImGui::TextDisabled("(content routing is only available on clip and");
        ImGui::TextDisabled("generative layers).");
        return;
    }

    // Title strip — show layer name / filepath so the user knows what
    // they're editing when the timeline + this window share focus.
    if (isClip) {
        const auto& clip = registry.get<Clip>(selected);
        ImGui::Text("Clip: %s", clip.filepath.c_str());
    } else {
        ImGui::Text("Generative layer (entity %u)",
                     static_cast<uint32_t>(selected));
    }
    ImGui::Separator();

    // Capture a working copy of the routing so we can edit it locally and
    // dispatch the whole thing via SetContentRoutingCommand on changes.
    // For undo simplicity each user action is one full-component replace.
    DraftRouting draft = captureDraft(registry, selected);

    // Mode dropdown.
    static const char* kModeLabels[] = {"Direct", "Tiled"};
    int modeIdx = (draft.mode == "Tiled") ? 1 : 0;
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::Combo("Mode", &modeIdx, kModeLabels, IM_ARRAYSIZE(kModeLabels))) {
        const std::string newMode = kModeLabels[modeIdx];
        DraftRouting next = draft;
        next.mode = newMode;
        if (newMode == "Direct") {
            // Direct collapses to a single target with identity uvRect.
            // Keep the first existing target if there is one; reset its
            // uvRect to identity so the dropdown semantic matches what a
            // user expects after switching back from Tiled.
            if (!next.targets.empty()) {
                next.targets.resize(1);
                next.targets[0].uvRect = {0.0f, 0.0f, 1.0f, 1.0f};
            }
        }
        dispatchDraft(m_dispatcher, m_timeline, selected, next);
        return;  // re-render on the next frame with the new mode
    }

    ImGui::SameLine();
    ImGui::TextDisabled("(%zu target%s)",
                         draft.targets.size(),
                         draft.targets.size() == 1 ? "" : "s");

    if (draft.mode == "Direct" && draft.targets.size() > 1) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                            "Direct mode uses the first target only.");
    }

    ImGui::Spacing();

    // Enumerate Screen entities for the per-row dropdown.
    std::vector<std::string> screenNames;
    screenNames.emplace_back("(all visible)");
    for (auto [_, s] : registry.view<Screen>().each()) {
        screenNames.push_back(s.name);
    }
    std::vector<const char*> screenNameCstrs;
    screenNameCstrs.reserve(screenNames.size());
    for (const auto& n : screenNames) screenNameCstrs.push_back(n.c_str());

    // Routing table.
    if (ImGui::BeginTable("##routes", 6,
                           ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Screen",  ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("UV X",    ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("UV Y",    ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("UV W",    ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("UV H",    ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(" ",        ImGuiTableColumnFlags_WidthFixed, 28.0f);
        ImGui::TableHeadersRow();

        std::optional<size_t> removeIdx;
        std::optional<DraftRouting> pendingDispatch;

        const bool tiled = (draft.mode == "Tiled");
        for (size_t ri = 0; ri < draft.targets.size(); ++ri) {
            ImGui::PushID(static_cast<int>(ri));
            ImGui::TableNextRow();

            // Screen picker.
            ImGui::TableSetColumnIndex(0);
            int screenIdx = 0;
            for (size_t i = 1; i < screenNames.size(); ++i) {
                if (screenNames[i] == draft.targets[ri].screenName) {
                    screenIdx = static_cast<int>(i);
                    break;
                }
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##screen", &screenIdx,
                              screenNameCstrs.data(),
                              static_cast<int>(screenNameCstrs.size()))) {
                DraftRouting next = draft;
                next.targets[ri].screenName =
                    (screenIdx == 0) ? std::string{} : screenNames[screenIdx];
                pendingDispatch = std::move(next);
            }

            // uvRect fields — only editable in Tiled mode. Direct mode
            // shows them disabled so the user can see why the values are
            // pinned at identity.
            auto editUv = [&](int col, int axis, const char* label) {
                ImGui::TableSetColumnIndex(col);
                float v = draft.targets[ri].uvRect[axis];
                ImGui::SetNextItemWidth(-1);
                if (!tiled) ImGui::BeginDisabled();
                const bool changed = ImGui::DragFloat(label, &v, 0.01f, -2.0f, 2.0f, "%.3f");
                if (!tiled) ImGui::EndDisabled();
                if (changed && tiled) {
                    DraftRouting next = draft;
                    next.targets[ri].uvRect[axis] = v;
                    pendingDispatch = std::move(next);
                }
            };
            editUv(1, 0, "##x");
            editUv(2, 1, "##y");
            editUv(3, 2, "##w");
            editUv(4, 3, "##h");

            // Row delete button.
            ImGui::TableSetColumnIndex(5);
            if (ImGui::Button("X", ImVec2(-1, 0))) {
                removeIdx = ri;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();

        if (removeIdx.has_value()) {
            DraftRouting next = draft;
            next.targets.erase(next.targets.begin() + static_cast<std::ptrdiff_t>(*removeIdx));
            // Removing the last target in Direct mode reverts to "all visible";
            // Tiled with zero targets is harmless (renders nothing this frame).
            pendingDispatch = std::move(next);
        }
        if (pendingDispatch.has_value()) {
            dispatchDraft(m_dispatcher, m_timeline, selected, *pendingDispatch);
        }
    }

    // Add-target controls.
    if (ImGui::Button("+ Add Target")) {
        DraftRouting next = draft;
        SetContentRoutingCommand::TargetSpec spec;
        // New rows default to "all visible" screen so the user sees a valid
        // (if unspecific) routing immediately. They pick the actual screen
        // from the dropdown in the row.
        spec.screenName = "";  // empty == "all visible" sentinel
        spec.uvRect = {0.0f, 0.0f, 1.0f, 1.0f};
        next.targets.push_back(std::move(spec));
        if (next.mode == "Direct" && next.targets.size() > 1) {
            // Adding a row implies Tiled authoring intent — switch mode
            // automatically so the user doesn't have to discover the dropdown.
            next.mode = "Tiled";
        }
        dispatchDraft(m_dispatcher, m_timeline, selected, next);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Empty screen name = 'render on all visible'.");
}

} // namespace entity
