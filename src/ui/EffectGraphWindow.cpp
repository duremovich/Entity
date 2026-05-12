#include "entity/ui/EffectGraphWindow.hpp"

#include "entity/components/Effect.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/effects/EffectKindRegistry.hpp"
#include "entity/timeline/Timeline.hpp"

#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <cstdint>
#include <string>

namespace ed = ax::NodeEditor;

namespace entity {

namespace {

// Synthetic node IDs that aren't backed by an entt::entity. We use the
// uint64 ID space above the entt::entity range so they never collide.
constexpr std::uint64_t kSyntheticLayerSourceNode = 0xFFFFFFFFFFFF0001ull;
constexpr std::uint64_t kSyntheticToScreenNode    = 0xFFFFFFFFFFFF0002ull;

// Pin IDs are derived from node IDs by adding a tag. Each node has at
// most one input + one output socket in v1, so we can pack:
//   pin_in  = (node << 2) | 1
//   pin_out = (node << 2) | 2
// node ID never has its low 2 bits set (entt::entity values + the two
// synthetic IDs both leave those bits free in practice — registries
// allocate sequentially from 0, and the synthetic IDs are 0xFFFF…0001/2
// which shift to safe values).
ed::PinId pinIn(std::uint64_t nodeId) {
    return ed::PinId{static_cast<uintptr_t>((nodeId << 2) | 1ull)};
}
ed::PinId pinOut(std::uint64_t nodeId) {
    return ed::PinId{static_cast<uintptr_t>((nodeId << 2) | 2ull)};
}

ImColor pinColorForKindCategory(const std::string& category) {
    if (category == "Color")    return ImColor( 90, 170, 255);
    if (category == "Stylize")  return ImColor(230, 190,  60);
    if (category == "User")     return ImColor(180, 120, 230);
    return ImColor(180, 180, 180);
}

} // namespace

EffectGraphWindow::EffectGraphWindow(Timeline* timeline)
    : m_timeline(timeline)
{
    ed::Config cfg;
    cfg.SettingsFile = nullptr;  // don't persist editor state to disk
    m_editorContext = ed::CreateEditor(&cfg);
}

EffectGraphWindow::~EffectGraphWindow() {
    if (m_editorContext) {
        ed::DestroyEditor(static_cast<ed::EditorContext*>(m_editorContext));
        m_editorContext = nullptr;
    }
}

void EffectGraphWindow::render() {
    if (!m_timeline) {
        ImGui::TextDisabled("No timeline.");
        return;
    }

    auto& registry = m_timeline->getRegistry();
    const entt::entity selected = m_timeline->getSelectedClip();

    if (selected == entt::null || !registry.valid(selected)) {
        ImGui::TextDisabled("Select a clip or generative layer to view its effect graph.");
        return;
    }

    auto* chain = registry.try_get<EffectChain>(selected);
    if (!chain || chain->nodes.empty()) {
        ImGui::Text("Selected layer has no effects.");
        ImGui::TextDisabled("Add one from the PropertyWindow's Effects section.");
        return;
    }

    ed::SetCurrentEditor(static_cast<ed::EditorContext*>(m_editorContext));
    ed::Begin("EffectGraphCanvas");

    // ----------------------------------------------------------------
    // Nodes
    // ----------------------------------------------------------------
    auto drawSyntheticNode = [](ed::NodeId id,
                                const char* label,
                                bool hasOutput, bool hasInput,
                                ImVec2 hint)
    {
        ed::SetNodePosition(id, hint);
        ed::BeginNode(id);
        ImGui::TextUnformatted(label);
        if (hasInput) {
            ed::BeginPin(pinIn(static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(id.AsPointer()))),
                          ed::PinKind::Input);
            ImGui::Text("-> in");
            ed::EndPin();
        }
        if (hasOutput) {
            ed::BeginPin(pinOut(static_cast<std::uint64_t>(reinterpret_cast<uintptr_t>(id.AsPointer()))),
                          ed::PinKind::Output);
            ImGui::Text("out ->");
            ed::EndPin();
        }
        ed::EndNode();
    };

    // Synthetic "Layer Source" — the chain's input feed.
    drawSyntheticNode(ed::NodeId{static_cast<uintptr_t>(kSyntheticLayerSourceNode)},
                      "Layer Source", true, false, ImVec2(40, 80));

    // One node per effect.
    int    idx     = 0;
    ImVec2 nextPos(220, 80);
    for (auto fxEnt : chain->nodes) {
        if (!registry.valid(fxEnt)) { ++idx; continue; }
        auto* fx = registry.try_get<Effect>(fxEnt);
        if (!fx) { ++idx; continue; }

        // Initial layout: lay out left-to-right if the user hasn't placed
        // the node yet (graphX == 0 && graphY == 0 is treated as "no
        // saved position").
        const std::uint64_t nodeIdU64 = static_cast<std::uint64_t>(fxEnt);
        ed::NodeId nid{static_cast<uintptr_t>(nodeIdU64)};
        if (fx->graphX == 0.0f && fx->graphY == 0.0f) {
            ed::SetNodePosition(nid, nextPos);
            nextPos.x += 200;
        } else {
            ed::SetNodePosition(nid, ImVec2(fx->graphX, fx->graphY));
        }

        ed::BeginNode(nid);

        // Header — display name from the registry if available.
        const effects::EffectKind* kind = m_effectKindRegistry
            ? m_effectKindRegistry->find(fx->kindId) : nullptr;
        const char* title = kind ? kind->displayName.c_str()
                                 : "(unknown effect)";
        ImGui::TextUnformatted(title);
        if (kind) {
            ImColor c = pinColorForKindCategory(kind->category);
            ImGui::PushStyleColor(ImGuiCol_Text, c.Value);
            ImGui::Text("%s", kind->category.c_str());
            ImGui::PopStyleColor();
        }

        // Pins: one Texture input, one Texture output. Parameter sockets
        // arrive in a follow-up.
        ed::BeginPin(pinIn(nodeIdU64), ed::PinKind::Input);
        ImGui::Text("-> in");
        ed::EndPin();
        ImGui::SameLine();
        ed::BeginPin(pinOut(nodeIdU64), ed::PinKind::Output);
        ImGui::Text("out ->");
        ed::EndPin();

        // Param value summary so the node is informative without making
        // the user open PropertyWindow for everything.
        if (kind) {
            const auto* params = registry.try_get<EffectParameters>(fxEnt);
            for (std::size_t s = 0; s < kind->params.size(); ++s) {
                const auto& schema = kind->params[s];
                if (schema.type != ParamValue::Type::Float) continue;
                const float v = (params && s < params->values.size())
                    ? params->values[s].f4[0]
                    : schema.defaultValue.f4[0];
                ImGui::Text("%s: %.2f", schema.displayName.c_str(), v);
            }
        }

        ed::EndNode();

        // After the layout pass, read back the canvas position and
        // persist it onto Effect so the next session restores it.
        // (Round-trip via project save happens through serialization;
        // the in-memory write keeps the view stable in-session.)
        const ImVec2 livePos = ed::GetNodePosition(nid);
        fx->graphX = livePos.x;
        fx->graphY = livePos.y;

        ++idx;
    }

    // Synthetic "To Screen" — final output sink.
    drawSyntheticNode(ed::NodeId{static_cast<uintptr_t>(kSyntheticToScreenNode)},
                      "To Screen", false, true, ImVec2(nextPos.x + 60, 80));

    // ----------------------------------------------------------------
    // Linear-chain edges. Phase 4 v1 doesn't yet support drag-connect;
    // edges are drawn as the implicit ordering of chain.nodes.
    // ----------------------------------------------------------------
    std::uint64_t prevNode = kSyntheticLayerSourceNode;
    int          linkCounter = 1;
    for (auto fxEnt : chain->nodes) {
        if (!registry.valid(fxEnt)) continue;
        const std::uint64_t nodeIdU64 = static_cast<std::uint64_t>(fxEnt);
        ed::Link(ed::LinkId{static_cast<uintptr_t>(linkCounter++)},
                  pinOut(prevNode), pinIn(nodeIdU64));
        prevNode = nodeIdU64;
    }
    // Tail link from last effect → To Screen.
    ed::Link(ed::LinkId{static_cast<uintptr_t>(linkCounter++)},
              pinOut(prevNode), pinIn(kSyntheticToScreenNode));

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

} // namespace entity
