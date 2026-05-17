#pragma once

#include "EditorWindow.hpp"

#include <entt/entt.hpp>
#include <imgui.h>

namespace entity {

class Timeline;
class CommandDispatcher;

/**
 * ContentRoutingWindow — Plane A content-routing library editor
 * (ADR-0022).
 *
 * Two-pane layout. Left: library list of ContentRoutingAsset entities
 * with "+ Add" affordance (Direct / Tiled — Feed Map lands in L3).
 * Right: detail editor for the selected asset (name, kind, targets,
 * Tiled wizard params, schematic preview).
 *
 * Auto-direct assets (`autoBoundScreen != null`) are name-synced and
 * lifecycle-tied to their Screen by RoutingLibrarySystem; the right
 * pane shows them with a read-only badge but still allows editing
 * (which silently breaks the autosync link in the system's next tick).
 *
 * Edits mutate the registry directly in L2 (no UndoableCommand
 * subclasses yet — those are L2b polish). PropertyWindow's Content
 * Routing dropdown is the primary "assign a clip to a routing"
 * affordance; this window is where the routings themselves get
 * authored.
 */
class ContentRoutingWindow : public EditorWindow {
public:
    explicit ContentRoutingWindow(Timeline* timeline);
    ~ContentRoutingWindow() override = default;

    void setCommandDispatcher(CommandDispatcher* dispatcher) { m_dispatcher = dispatcher; }

    void render() override;
    const char* getName() const override { return "Content Routing"; }
    ImGuiWindowFlags getWindowFlags() const override { return ImGuiWindowFlags_None; }

private:
    Timeline* m_timeline{nullptr};
    CommandDispatcher* m_dispatcher{nullptr};

    // Currently-selected library asset entity. Window-local UI state —
    // not persisted across sessions. entt::null = "nothing selected".
    entt::entity m_selectedAsset{entt::null};

    // Set when the user clicks Delete on an asset; consumed by the next
    // frame's confirmation popup. The popup walks ContentRoutingRef
    // view to count usage.
    entt::entity m_pendingDelete{entt::null};
};

} // namespace entity
