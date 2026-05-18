#pragma once

#include "entity/ui/ContentRoutingWindow.hpp"

#include <entt/entt.hpp>
#include <imgui.h>

namespace entity {

struct ContentRoutingAsset;

/**
 * Render the Plane-A routing canvas (works for any RouteMode kind).
 *
 * Shared between ContentRoutingWindow's small in-pane preview and
 * FeedMapEditorWindow's full-window precision editor. Mutates
 * asset.targets[i].uvRect during drag; caller passes mutable
 * references for the persistent state (drag + viewport) so the
 * function stays stateless across frames.
 *
 * availSize is the box the canvas fits into. Internally:
 *   - preserves the source aspect ratio,
 *   - floors at 120x90 so a tiny pane doesn't collapse it,
 *   - centers horizontally inside availSize,
 *   - wraps everything in an inner BeginChild so wheel events stay
 *     with the canvas rather than bubbling to the parent's scroll.
 *
 * texId is the optional poster-frame texture id (from the
 * selected clip's VideoTexture). Pass nullptr for a flat-panel
 * background.
 */
void drawContentRoutingCanvas(
    entt::registry& reg,
    ContentRoutingAsset& asset,
    void* texId,
    ContentRoutingWindow::CanvasDragState& dragState,
    ContentRoutingWindow::CanvasViewport& view,
    ImVec2 availSize);

} // namespace entity
