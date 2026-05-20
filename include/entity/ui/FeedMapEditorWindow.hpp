#pragma once

#include "EditorWindow.hpp"
#include "entity/ui/ContentRoutingWindow.hpp"

#include <entt/entt.hpp>

namespace entity {

class Timeline;
class CommandDispatcher;
class IRenderer;

/**
 * FeedMapEditorWindow — dedicated, dockable editor for Feed Map
 * content-routing assets (ADR-0022, follow-up).
 *
 * Background: the Feed Map authoring canvas docked inside the
 * Content Routing window's right pane was too small for precise
 * region placement — even with cursor-anchored zoom, the visible
 * area was capped by the pane's width. This window provides the
 * dedicated workspace pattern common to feed-mapping tools, with a
 * sidebar of region/source controls on the left and a canvas filling
 * the rest.
 *
 * Defaults to hidden. Made visible by:
 *   - Windows menu toggle
 *   - ContentRoutingWindow's "Edit in Feed Map Editor..." button
 *     (which also calls editAsset to focus the routed asset)
 *
 * Visibility persists per workspace via the same hiddenWindows
 * mechanism every other EditorWindow uses.
 */
class FeedMapEditorWindow : public EditorWindow {
public:
    explicit FeedMapEditorWindow(Timeline* timeline);
    ~FeedMapEditorWindow() override = default;

    void setCommandDispatcher(CommandDispatcher* dispatcher) { m_dispatcher = dispatcher; }
    void setRenderer(IRenderer* renderer) { m_renderer = renderer; }

    void render() override;
    const char* getName() const override { return "Feed Map Editor"; }

    // Reasonable default size on first appearance — undocked the
    // window opens as a 1200x700 floater so the canvas has room.
    // Docked, ImGui uses the dock node's size and ignores this.
    void applyPreBeginStyles() const override;

    // Select an asset and make the window visible. Called from
    // ContentRoutingWindow when the user clicks "Edit in Feed Map
    // Editor...". Safe to call with entt::null (clears selection).
    void editAsset(entt::entity asset);

private:
    Timeline* m_timeline{nullptr};
    CommandDispatcher* m_dispatcher{nullptr};
    IRenderer* m_renderer{nullptr};

    // Selected FeedMap asset — independent of ContentRoutingWindow's
    // selection so the user can edit one feed map here while browsing
    // the library there.
    entt::entity m_selectedAsset{entt::null};

    // Canvas state — separate from ContentRoutingWindow's so the two
    // canvases don't fight over drag / zoom / pan.
    ContentRoutingWindow::CanvasDragState m_drag{};
    ContentRoutingWindow::CanvasViewport m_view{};

    // Set on the frame editAsset() is called; the next render() makes
    // the window visible and clears the flag. Two-step so we don't
    // touch m_visible from outside the WindowManager render loop.
    bool m_pendingFocus{false};
};

} // namespace entity
