#include "entity/ui/EffectGraphWindow.hpp"

#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include "entity/components/Effect.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/components/EffectParameters.hpp"
#include "entity/effects/EffectChainTopo.hpp"
#include "entity/effects/EffectKindRegistry.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/ui/EffectUiCommon.hpp"

#include <imgui.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ed = ax::NodeEditor;

namespace entity {

namespace {

// Synthetic node IDs that aren't backed by an entt::entity. Kept inside
// 60 bits so the << 4 in the pin encoding below never truncates them.
constexpr std::uint64_t kSyntheticLayerSourceNode = 0x0FFFFFFFFFFF0001ull;
constexpr std::uint64_t kSyntheticToScreenNode    = 0x0FFFFFFFFFFF0002ull;

// Pin encoding: (node << 4) | (isOutput ? 0x8 : 0) | socketIndex.
// Supports up to 8 sockets per direction — assert-guarded at the call
// sites. Kept in ONE place (these two helpers + decodePin) so the
// encode/decode can never diverge.
ed::PinId pinIn(std::uint64_t nodeId, int socket = 0) {
    return ed::PinId{static_cast<uintptr_t>(
        (nodeId << 4) | static_cast<std::uint64_t>(socket & 0x7))};
}
ed::PinId pinOut(std::uint64_t nodeId, int socket = 0) {
    return ed::PinId{static_cast<uintptr_t>(
        (nodeId << 4) | 0x8ull | static_cast<std::uint64_t>(socket & 0x7))};
}

struct PinInfo {
    std::uint64_t node{0};
    bool          isOutput{false};
    int           socket{0};
};
PinInfo decodePin(ed::PinId id) {
    const auto v = static_cast<std::uint64_t>(id.Get());
    return {v >> 4, (v & 0x8ull) != 0, static_cast<int>(v & 0x7ull)};
}

ImColor pinColorForKindCategory(const std::string& category) {
    if (category == "Color")    return ImColor( 90, 170, 255);
    if (category == "Stylize")  return ImColor(230, 190,  60);
    if (category == "User")     return ImColor(180, 120, 230);
    return ImColor(180, 180, 180);
}

// Role styling: derived from the sockets, same rule as the engine
// (EffectKind::textureInputCount).
ImColor roleColor(const effects::EffectKind* kind) {
    if (!kind) return ImColor(180, 180, 180);
    const int inputs = kind->textureInputCount();
    if (inputs == 0) return ImColor( 90, 200, 120);  // generator: green
    if (inputs >= 2) return ImColor(230, 160,  60);  // combiner: amber
    return pinColorForKindCategory(kind->category);   // filter: category
}

// Texture input/output sockets of a kind, in declaration order. Unknown
// kind degrades to one unnamed in + out (matches the engine default).
struct SocketLists {
    std::vector<const effects::SocketSchema*> inputs;
    std::vector<const effects::SocketSchema*> outputs;
};
SocketLists socketsOf(const effects::EffectKind* kind) {
    SocketLists out;
    if (!kind) return out;
    for (const auto& s : kind->sockets) {
        if (s.kind != effects::SocketSchema::Kind::Texture) continue;
        if (s.direction == effects::SocketSchema::Direction::Input) {
            out.inputs.push_back(&s);
        } else {
            out.outputs.push_back(&s);
        }
    }
    return out;
}

} // namespace

EffectGraphWindow::EffectGraphWindow(Timeline* timeline)
    : m_timeline(timeline)
{
    ed::Config cfg;
    cfg.SettingsFile = nullptr;  // don't persist editor state to disk
    // Default FitVerticalView re-fits the view on every canvas-size
    // change, which combined with docking/floating size jitter drifts
    // the editor's persistent view until content lands in the top-left
    // and eventually clips out. CenterOnly only re-centers pan; scale
    // is preserved.
    cfg.CanvasSizeMode = ed::CanvasSizeMode::CenterOnly;
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
        ImGui::TextDisabled("Add one from the PropertyWindow's Effects section\n"
                            "or right-click the canvas below.");
    }

    // When the user switches to a different layer, the previous chain's
    // node IDs are no longer relevant. Drop them so the set stays bounded
    // across a session.
    if (selected != m_lastSelectedClip) {
        m_placedNodes.clear();
        m_lastSelectedClip = selected;
    }

    // imgui-node-editor's canvas miscomputes its widget rect (renders
    // into a tiny sub-rect at the panel's top-left, with the rest of
    // the panel showing the app background) when ed::Begin is the first
    // ImGui item emitted after the host ImGui::Begin. Emitting any item
    // beforehand resolves it — the canvas then fills the panel and
    // tracks resize / undock / drag correctly. Found empirically; using
    // a hint line so it doubles as UX guidance instead of an invisible
    // spacer.
    ImGui::TextDisabled("Pan: right-click drag.  Zoom: mouse wheel.  "
                        "Drag pins to connect.  Right-click: add / edit.");

    ed::SetCurrentEditor(static_cast<ed::EditorContext*>(m_editorContext));
    // No explicit size — the library samples GetContentRegionAvail()
    // itself with ImFloor for stability, matching the upstream
    // simple-example / blueprints-example wrappers.
    ed::Begin("EffectGraphCanvas");

    // SetNodePosition is an authoritative override in imgui-node-editor —
    // it must only be called for the *initial* placement of each node.
    // Calling it every frame overwrites the editor's drag state and
    // destabilises the viewport's zoom transform.
    auto placeOnce = [this](std::uint64_t key, ed::NodeId id, ImVec2 initialPos) {
        if (m_placedNodes.insert(key).second) {
            ed::SetNodePosition(id, initialPos);
        }
    };

    // ----------------------------------------------------------------
    // Nodes
    // ----------------------------------------------------------------
    auto drawSyntheticNode = [&](std::uint64_t key,
                                 const char* label,
                                 bool hasOutput, bool hasInput,
                                 ImVec2 hint)
    {
        ed::NodeId id{static_cast<uintptr_t>(key)};
        placeOnce(key, id, hint);
        ed::BeginNode(id);
        ImGui::TextUnformatted(label);
        if (hasInput) {
            ed::BeginPin(pinIn(key), ed::PinKind::Input);
            ImGui::Text("-> in");
            ed::EndPin();
        }
        if (hasOutput) {
            ed::BeginPin(pinOut(key), ed::PinKind::Output);
            ImGui::Text("out ->");
            ed::EndPin();
        }
        ed::EndNode();
    };

    // Synthetic "Layer Source" — the chain's input feed.
    drawSyntheticNode(kSyntheticLayerSourceNode,
                      "Layer Source", true, false, ImVec2(40, 80));

    // One node per effect.
    ImVec2 nextPos(220, 80);
    if (chain) {
        for (auto fxEnt : chain->nodes) {
            if (!registry.valid(fxEnt)) continue;
            auto* fx = registry.try_get<Effect>(fxEnt);
            if (!fx) continue;

            const std::uint64_t nodeIdU64 = static_cast<std::uint64_t>(fxEnt);
            ed::NodeId nid{static_cast<uintptr_t>(nodeIdU64)};

            // Initial position: saved (graphX/Y) if non-zero, else the
            // left-to-right auto-layout cursor. Only advance the cursor when
            // we actually consume it, so a mixed saved/unsaved chain still
            // spaces the unsaved nodes correctly.
            const bool hasSavedPos = (fx->graphX != 0.0f || fx->graphY != 0.0f);
            ImVec2 initialPos = hasSavedPos ? ImVec2(fx->graphX, fx->graphY) : nextPos;
            placeOnce(nodeIdU64, nid, initialPos);
            if (!hasSavedPos) {
                nextPos.x += 200;
            }

            const bool dimmed = !fx->enabled;
            if (dimmed) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);

            ed::BeginNode(nid);

            // Header — display name + role-colored category line.
            const effects::EffectKind* kind = m_effectKindRegistry
                ? m_effectKindRegistry->find(fx->kindId) : nullptr;
            const char* title = kind ? kind->displayName.c_str()
                                     : "(unknown effect)";
            ImGui::TextUnformatted(title);
            if (dimmed) {
                ImGui::SameLine();
                ImGui::TextDisabled("[off]");
            }
            if (kind) {
                ImColor c = roleColor(kind);
                ImGui::PushStyleColor(ImGuiCol_Text, c.Value);
                ImGui::Text("%s", kind->category.c_str());
                ImGui::PopStyleColor();
            }

            // Pins from the kind's SocketSchema — inputs left, outputs
            // right, combiners get one input pin per texture socket.
            const SocketLists sockets = socketsOf(kind);
            const int inCount  = kind ? static_cast<int>(sockets.inputs.size())  : 1;
            const int outCount = kind ? static_cast<int>(sockets.outputs.size()) : 1;
            IM_ASSERT(inCount <= 8 && outCount <= 8);
            const int rows = (inCount > outCount) ? inCount : outCount;
            for (int r = 0; r < rows; ++r) {
                bool drewInput = false;
                if (r < inCount) {
                    const char* name = (kind && r < static_cast<int>(sockets.inputs.size()))
                        ? sockets.inputs[static_cast<std::size_t>(r)]->name.c_str()
                        : "in";
                    ed::BeginPin(pinIn(nodeIdU64, r), ed::PinKind::Input);
                    ImGui::Text("-> %s", name);
                    ed::EndPin();
                    drewInput = true;
                }
                if (r < outCount) {
                    if (drewInput) ImGui::SameLine();
                    const char* name = (kind && r < static_cast<int>(sockets.outputs.size()))
                        ? sockets.outputs[static_cast<std::size_t>(r)]->name.c_str()
                        : "out";
                    ed::BeginPin(pinOut(nodeIdU64, r), ed::PinKind::Output);
                    ImGui::Text("%s ->", name);
                    ed::EndPin();
                }
            }

            // Param value summary so the node is informative without making
            // the user open PropertyWindow for everything. Float prints the
            // number, Color shows a swatch, Enum its label; other types are
            // skipped (edit them in PropertyWindow).
            if (kind) {
                const auto* params = registry.try_get<EffectParameters>(fxEnt);
                for (std::size_t s = 0; s < kind->params.size(); ++s) {
                    const auto& schema = kind->params[s];
                    const ParamValue v = (params && s < params->values.size())
                        ? params->values[s]
                        : schema.defaultValue;
                    switch (schema.type) {
                        case ParamValue::Type::Float:
                            ImGui::Text("%s: %.2f", schema.displayName.c_str(), v.f4[0]);
                            break;
                        case ParamValue::Type::Color: {
                            ImGui::Text("%s:", schema.displayName.c_str());
                            ImGui::SameLine();
                            // Non-interactive swatch (a real picker popup can't
                            // live inside the node-editor canvas).
                            ImGui::ColorButton("##swatch",
                                               ImVec4(v.f4[0], v.f4[1], v.f4[2], v.f4[3]),
                                               ImGuiColorEditFlags_NoTooltip |
                                               ImGuiColorEditFlags_NoPicker,
                                               ImVec2(28, 14));
                            break;
                        }
                        case ParamValue::Type::Enum: {
                            const int idx = v.i;
                            const char* label =
                                (idx >= 0 && idx < static_cast<int>(schema.enumLabels.size()))
                                    ? schema.enumLabels[static_cast<std::size_t>(idx)].c_str()
                                    : "?";
                            ImGui::Text("%s: %s", schema.displayName.c_str(), label);
                            break;
                        }
                        default:
                            break;
                    }
                }
            }

            ed::EndNode();
            if (dimmed) ImGui::PopStyleVar();

            // Read back the live position and mirror it onto the Effect
            // component so project save persists where the user dragged it.
            // Guard the write so we don't dirty the project every frame when
            // nothing has moved.
            const ImVec2 livePos = ed::GetNodePosition(nid);
            if (livePos.x != fx->graphX || livePos.y != fx->graphY) {
                fx->graphX = livePos.x;
                fx->graphY = livePos.y;
            }
        }
    }

    // Synthetic "To Screen" — final output sink.
    drawSyntheticNode(kSyntheticToScreenNode,
                      "To Screen", false, true, ImVec2(nextPos.x + 60, 80));

    // ----------------------------------------------------------------
    // Links. Explicit topology when the chain carries connections;
    // otherwise the implicit linear chain of chain->nodes. Link IDs are
    // rebuilt per frame: explicit links use connectionIndex+1 (queries
    // resolve same-frame so index-as-ID is safe); implicit links use a
    // parallel counter offset far above so the two spaces can't collide
    // mid-edit.
    // ----------------------------------------------------------------
    constexpr uintptr_t kImplicitLinkBase = 1u << 20;
    const bool explicitTopology = chain && !chain->connections.empty();
    if (chain) {
        if (explicitTopology) {
            for (std::size_t ci = 0; ci < chain->connections.size(); ++ci) {
                const auto& c = chain->connections[ci];
                const std::uint64_t srcKey = (c.srcNode == entt::null)
                    ? kSyntheticLayerSourceNode
                    : static_cast<std::uint64_t>(c.srcNode);
                const std::uint64_t dstKey = (c.dstNode == entt::null)
                    ? kSyntheticToScreenNode
                    : static_cast<std::uint64_t>(c.dstNode);
                ed::Link(ed::LinkId{static_cast<uintptr_t>(ci + 1)},
                         pinOut(srcKey, c.srcSocket),
                         pinIn(dstKey, c.dstSocket));
            }
        } else {
            std::uint64_t prevNode = kSyntheticLayerSourceNode;
            uintptr_t linkCounter = kImplicitLinkBase;
            for (auto fxEnt : chain->nodes) {
                if (!registry.valid(fxEnt)) continue;
                const std::uint64_t nodeIdU64 = static_cast<std::uint64_t>(fxEnt);
                ed::Link(ed::LinkId{linkCounter++}, pinOut(prevNode), pinIn(nodeIdU64));
                prevNode = nodeIdU64;
            }
            ed::Link(ed::LinkId{linkCounter++},
                     pinOut(prevNode), pinIn(kSyntheticToScreenNode));
        }
    }

    // ----------------------------------------------------------------
    // Interactions — all mutations go through undoable commands.
    // ----------------------------------------------------------------
    auto keyToEntity = [](std::uint64_t key) -> entt::entity {
        if (key == kSyntheticLayerSourceNode || key == kSyntheticToScreenNode) {
            return entt::null;
        }
        return static_cast<entt::entity>(key);
    };

    // Drag-connect.
    if (chain && ed::BeginCreate()) {
        ed::PinId startPin, endPin;
        if (ed::QueryNewLink(&startPin, &endPin)) {
            const PinInfo a = decodePin(startPin);
            const PinInfo b = decodePin(endPin);

            auto rejectWith = [&](const char* why) {
                ed::RejectNewItem(ImColor(220, 60, 60), 2.0f);
                ed::Suspend();
                ImGui::SetTooltip("%s", why);
                ed::Resume();
            };

            if (a.node == b.node) {
                rejectWith("Cannot connect a node to itself");
            } else if (a.isOutput == b.isOutput) {
                rejectWith(a.isOutput ? "Both pins are outputs"
                                      : "Both pins are inputs");
            } else {
                // Normalize direction: out -> in.
                const PinInfo& outPin = a.isOutput ? a : b;
                const PinInfo& inPin  = a.isOutput ? b : a;
                const entt::entity srcNode = keyToEntity(outPin.node);
                const entt::entity dstNode = keyToEntity(inPin.node);

                if (outPin.node == kSyntheticToScreenNode ||
                    inPin.node == kSyntheticLayerSourceNode) {
                    rejectWith("Layer Source only feeds; To Screen only receives");
                } else {
                    // Cycle pre-check against the topology the command will
                    // edit (materialized linear if none yet). The command
                    // re-checks at execute; this is the UX tooltip.
                    bool wouldCycle = false;
                    if (srcNode != entt::null && dstNode != entt::null) {
                        if (chain->connections.empty()) {
                            EffectChain probe = *chain;
                            probe.connections =
                                effects::materializeLinearTopology(*chain);
                            wouldCycle = effects::topologyWouldCycle(
                                probe, srcNode, dstNode);
                        } else {
                            wouldCycle = effects::topologyWouldCycle(
                                *chain, srcNode, dstNode);
                        }
                    }
                    if (wouldCycle) {
                        rejectWith("Would create a cycle");
                    } else if (ed::AcceptNewItem()) {
                        if (m_dispatcher) {
                            m_dispatcher->enqueue(
                                std::make_unique<ConnectEffectCommand>(
                                    selected, srcNode, dstNode,
                                    static_cast<std::uint8_t>(outPin.socket),
                                    static_cast<std::uint8_t>(inPin.socket)));
                        }
                    }
                }
            }
        }
    }
    ed::EndCreate();

    // Delete links / nodes (Del key or right-click delete on selection).
    if (chain && ed::BeginDelete()) {
        ed::LinkId deletedLink;
        while (ed::QueryDeletedLink(&deletedLink)) {
            const auto raw = static_cast<uintptr_t>(deletedLink.Get());
            if (explicitTopology && raw >= 1 && raw <= chain->connections.size()) {
                const auto& c = chain->connections[raw - 1];
                if (ed::AcceptDeletedItem()) {
                    if (m_dispatcher) {
                        m_dispatcher->enqueue(
                            std::make_unique<DisconnectEffectCommand>(
                                selected, c.dstNode, c.dstSocket));
                    }
                }
            } else if (!explicitTopology && raw >= kImplicitLinkBase) {
                // Deleting an implicit link: materialize the linear
                // topology minus that link in one undoable step.
                const std::size_t li = raw - kImplicitLinkBase;
                auto conns = effects::materializeLinearTopology(*chain);
                if (li < conns.size() && ed::AcceptDeletedItem()) {
                    conns.erase(conns.begin() + static_cast<std::ptrdiff_t>(li));
                    if (m_dispatcher) {
                        m_dispatcher->enqueue(
                            std::make_unique<SetEffectGraphTopologyCommand>(
                                selected, std::move(conns), chain->outputNode));
                    }
                } else {
                    ed::RejectDeletedItem();
                }
            } else {
                ed::RejectDeletedItem();
            }
        }
        ed::NodeId deletedNode;
        while (ed::QueryDeletedNode(&deletedNode)) {
            const auto key = static_cast<std::uint64_t>(deletedNode.Get());
            if (key == kSyntheticLayerSourceNode || key == kSyntheticToScreenNode) {
                ed::RejectDeletedItem();
                continue;
            }
            if (ed::AcceptDeletedItem()) {
                if (m_dispatcher) {
                    m_dispatcher->enqueue(std::make_unique<RemoveEffectCommand>(
                        selected, static_cast<entt::entity>(key)));
                }
            }
        }
    }
    ed::EndDelete();

    // Context menus. Popups must run outside the canvas transform.
    ed::Suspend();
    if (ed::ShowBackgroundContextMenu()) {
        m_contextCanvasPos = ed::ScreenToCanvas(ImGui::GetMousePos());
        ImGui::OpenPopup("GraphAddEffect");
    }
    ed::NodeId contextNode;
    if (ed::ShowNodeContextMenu(&contextNode)) {
        const auto key = static_cast<std::uint64_t>(contextNode.Get());
        if (key != kSyntheticLayerSourceNode && key != kSyntheticToScreenNode) {
            m_contextNode = static_cast<entt::entity>(key);
            ImGui::OpenPopup("GraphNodeMenu");
        }
    }

    if (ImGui::BeginPopup("GraphAddEffect")) {
        ui::renderEffectKindMenu(m_effectKindRegistry,
            [&](const effects::EffectKind& k) {
                if (!m_dispatcher) return;
                auto cmd = std::make_unique<AddEffectCommand>(selected, k.stableId);
                cmd->setInitialGraphPos(m_contextCanvasPos.x, m_contextCanvasPos.y);
                m_dispatcher->enqueue(std::move(cmd));
            });
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("GraphNodeMenu")) {
        auto* fx = registry.valid(m_contextNode)
            ? registry.try_get<Effect>(m_contextNode) : nullptr;
        if (fx) {
            if (ImGui::MenuItem(fx->enabled ? "Disable" : "Enable")) {
                if (m_dispatcher) {
                    auto cmd = std::make_unique<SetEffectEnabledCommand>(
                        m_contextNode, !fx->enabled);
                    cmd->setPreviousEnabled(fx->enabled);
                    m_dispatcher->enqueue(std::move(cmd));
                }
            }
            const bool isOutput = chain && chain->outputNode == m_contextNode;
            if (ImGui::MenuItem("Set as output", nullptr, isOutput, !isOutput)) {
                // Wire node -> To Screen (replaces the incumbent output
                // link and re-points outputNode; one undo step).
                if (m_dispatcher) {
                    m_dispatcher->enqueue(std::make_unique<ConnectEffectCommand>(
                        selected, m_contextNode, entt::null, 0, 0));
                }
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete")) {
                if (m_dispatcher) {
                    m_dispatcher->enqueue(std::make_unique<RemoveEffectCommand>(
                        selected, m_contextNode));
                }
            }
        } else {
            ImGui::TextDisabled("(node no longer exists)");
        }
        ImGui::EndPopup();
    }
    ed::Resume();

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

} // namespace entity
