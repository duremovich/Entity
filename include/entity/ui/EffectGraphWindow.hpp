#pragma once

#include "EditorWindow.hpp"

#include <entt/entt.hpp>

#include <cstdint>
#include <unordered_set>

namespace entity {

class Timeline;
class CommandDispatcher;
namespace effects { class EffectKindRegistry; }

/**
 * EffectGraphWindow - Edits a layer's effect chain as a node graph
 * (issue #54, Phase 4).
 *
 * Shipped:
 *   - Synthetic "Layer Source" → effect nodes → "To Screen"; pins
 *     rendered from each kind's SocketSchema (combiners show two
 *     texture inputs). Node drag persists graphX / graphY.
 *   - Drag-connect (ConnectEffectCommand — materializes the implicit
 *     linear chain on first edit, replaces occupied inputs, refuses
 *     cycles with a tooltip), link/node deletion, background
 *     context-menu add-at-cursor, node menu (enable/disable, set as
 *     output, delete). Every mutation is an undoable command.
 *   - Role styling: generators green, combiners amber, filters by
 *     category; disabled nodes dim with an ASCII "[off]" tag.
 *
 * Deferred to follow-up:
 *   - Selection routing back to PropertyWindow (focused-effect view)
 *   - Custom socket types (Float / Color sockets feeding param inputs)
 *
 * Implementation note: uses `unofficial::imgui-node-editor` vendored via
 * vcpkg. One persistent editor context per window; the context owns its
 * own viewport pan / zoom state.
 */
class EffectGraphWindow : public EditorWindow {
public:
    explicit EffectGraphWindow(Timeline* timeline);
    ~EffectGraphWindow() override;

    EffectGraphWindow(const EffectGraphWindow&) = delete;
    EffectGraphWindow& operator=(const EffectGraphWindow&) = delete;

    void setCommandDispatcher(CommandDispatcher* dispatcher) {
        m_dispatcher = dispatcher;
    }
    void setEffectKindRegistry(const effects::EffectKindRegistry* reg) {
        m_effectKindRegistry = reg;
    }

    void render() override;
    const char* getName() const override { return "Effect Graph"; }

    // imgui-node-editor needs the mouse wheel; without these flags the
    // host ImGui window scrolls on wheel and competes with the editor's
    // zoom. Do NOT override WindowPadding here — pushing it to (0,0)
    // misaligns the editor's content-rect computation inside a docked
    // tab and the canvas ends up rendered into a sub-rect of the panel.
    ImGuiWindowFlags getWindowFlags() const override {
        return ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    }

private:
    Timeline*                              m_timeline{nullptr};
    CommandDispatcher*                     m_dispatcher{nullptr};
    const effects::EffectKindRegistry*     m_effectKindRegistry{nullptr};

    // Type-erased pointer to ax::NodeEditor::EditorContext — pulled in as
    // an opaque void* so the header doesn't drag imgui-node-editor into
    // every translation unit that includes this. The .cpp casts back.
    void* m_editorContext{nullptr};

    // Tracks which node IDs we've already handed to ed::SetNodePosition.
    // imgui-node-editor treats SetNodePosition as an authoritative
    // override; calling it every frame fights the editor's drag state
    // and viewport math, so we place each node exactly once. Entity
    // values include EnTT version bits, so a destroyed-then-recreated
    // effect at the same numeric ID gets a fresh placement.
    std::unordered_set<std::uint64_t> m_placedNodes;
    entt::entity                      m_lastSelectedClip{entt::null};

    // Context-menu state: canvas position of the background right-click
    // (new nodes land there) and the node the node-menu targets.
    ImVec2       m_contextCanvasPos{0.0f, 0.0f};
    entt::entity m_contextNode{entt::null};
};

} // namespace entity
