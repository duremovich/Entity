#pragma once

#include "EditorWindow.hpp"

#include <entt/entt.hpp>
#include <imgui.h>

namespace entity {

class Timeline;
class CommandDispatcher;
class IRenderer;
class FeedMapEditorWindow;

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

    // Optional pointer to the dedicated Feed Map editor. When wired,
    // the Feed Map detail pane exposes an "Edit in Feed Map Editor..."
    // button that focuses the editor on the selected asset. Null is
    // fine — the button just hides itself.
    void setFeedMapEditor(FeedMapEditorWindow* editor) { m_feedMapEditor = editor; }

    void render() override;
    const char* getName() const override { return "Content Routing"; }
    ImGuiWindowFlags getWindowFlags() const override { return ImGuiWindowFlags_None; }

    // Which part of a region is being dragged. Corners + edge
    // midpoints + body translation. Order matches a 3x3 grid (Body
    // is the center) so iteration in the renderer stays readable.
    enum class CanvasHandle : int {
        Body = 0,
        NW, N, NE,
        W,      E,
        SW, S, SE,
    };

    // ADR-0022 L5: per-frame canvas drag state. Lives on the window
    // across frames so a mouse-down on a region body / corner persists
    // through the drag without re-hit-testing every frame.
    struct CanvasDragState {
        int regionIdx{-1};                            // -1 = no drag in progress
        CanvasHandle handle{CanvasHandle::Body};      // which handle is active
    };

    // Floating-source viewport state (Photoshop/Figma model). The
    // canvas surface IS the full pane; the source rect floats inside
    // it. panScreen is the screen-pixel offset of the source's
    // top-left within the editor area. zoom is source-pixels →
    // screen-pixels ratio (zoom=2.0 means each source pixel covers
    // 2 screen pixels). zoom=0 is a sentinel that triggers auto-fit
    // on the next draw (centered with a margin so the source isn't
    // flush against the edges). Ephemeral — not persisted.
    struct CanvasViewport {
        ImVec2 panScreen{0.0f, 0.0f};
        float zoom{0.0f};
    };

private:
    Timeline* m_timeline{nullptr};
    CommandDispatcher* m_dispatcher{nullptr};
    IRenderer* m_renderer{nullptr};
    FeedMapEditorWindow* m_feedMapEditor{nullptr};

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

    // Canvas zoom + pan. Reset by the "Fit" button and whenever the
    // user edits source canvas dimensions (aspect change otherwise
    // leaves the viewport in a weird state).
    CanvasViewport m_canvasView{};

    // Right pane's vertical split between the upper controls area
    // (name, kind, extras, targets table) and the lower canvas
    // preview. <=0 means "auto-initialize on first draw" (picks a
    // sensible default once we know the pane height). Adjusted by
    // dragging the horizontal splitter between the two regions.
    float m_upperPaneHeight{0.0f};
};

} // namespace entity
