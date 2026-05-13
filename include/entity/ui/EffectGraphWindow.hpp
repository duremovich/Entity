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
 * EffectGraphWindow - Visualises a layer's effect chain as a node graph
 * (issue #54, Phase 4).
 *
 * v1 scope:
 *   - Reads the currently-selected layer's EffectChain via Timeline (same
 *     pattern as PropertyWindow).
 *   - Renders synthetic "Layer Source" → effect nodes → "To Screen"
 *     pins, with linear-chain edges drawn between them.
 *   - Node drag-to-move updates each effect's graphX / graphY on the
 *     fly (live in the component; persistence to project files happens
 *     via the existing serialization layer).
 *
 * Deferred to follow-up:
 *   - Drag-connect / disconnect, cycle detection, branching topology
 *   - Selection routing back to PropertyWindow (focused-effect view)
 *   - Custom socket types (Float / Color sockets feeding param inputs)
 *   - Right-click context menu (insert effect, delete, duplicate)
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
};

} // namespace entity
