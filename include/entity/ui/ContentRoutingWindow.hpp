#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>

namespace entity {

class Timeline;
class CommandDispatcher;

/**
 * ContentRoutingWindow — Plane A content-routing editor (ADR-0021 M3).
 *
 * Edits the ContentRouting component on the selected clip / generative
 * layer. Direct mode = single target screen with implicit identity uvRect
 * (matches the legacy `clip.targetScreen` dropdown in PropertyWindow).
 * Tiled mode = multi-target with per-target UV crop, for feed-map slicing
 * (one source carved across several screens).
 *
 * All edits route through `SetContentRoutingCommand` for undo and
 * editor-thread affinity. When no Engine / dispatcher is wired (headless
 * test paths), edits are inert.
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
};

} // namespace entity
