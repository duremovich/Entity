#pragma once

#include "EditorWindow.hpp"

#include <entt/entt.hpp>
#include <imgui.h>

namespace entity {

class Timeline;
class CommandDispatcher;
class IRenderer;

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

    // ADR-0022 L5: when wired, the canvas preview samples the
    // currently-selected clip's video texture as the background. Null
    // is fine — the canvas falls back to the schematic-only path.
    void setRenderer(IRenderer* renderer) { m_renderer = renderer; }

    void render() override;
    const char* getName() const override { return "Content Routing"; }
    ImGuiWindowFlags getWindowFlags() const override { return ImGuiWindowFlags_None; }

    // ADR-0022 L5: per-frame canvas drag state. Lives on the window
    // across frames so a mouse-down on a region body / corner persists
    // through the drag without re-hit-testing every frame. handle
    // encodes which part of the region is being dragged. Public so
    // the .cpp's free helper functions can reference the type.
    struct CanvasDragState {
        int regionIdx{-1};       // -1 = no drag in progress
        int handle{0};           // 0 = body, 1=NW, 2=NE, 3=SW, 4=SE
    };

private:
    Timeline* m_timeline{nullptr};
    CommandDispatcher* m_dispatcher{nullptr};
    IRenderer* m_renderer{nullptr};

    // Currently-selected library asset entity. Window-local UI state —
    // not persisted across sessions. entt::null = "nothing selected".
    entt::entity m_selectedAsset{entt::null};

    // Set when the user clicks Delete on an asset; consumed by the next
    // frame's confirmation popup. The popup walks ContentRoutingRef
    // view to count usage.
    entt::entity m_pendingDelete{entt::null};

    // Drag-edit state for the canvas (L5). Reset when no mouse button
    // is held; regionIdx stays >= 0 while a drag is in progress.
    CanvasDragState m_canvasDrag{};
};

} // namespace entity
