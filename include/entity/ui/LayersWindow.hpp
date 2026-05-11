#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>
#include <entt/entt.hpp>

namespace entity {

class Engine;

// LayersWindow — palette of layer kinds the user can drag onto the timeline.
//
// Lists "Clip" and "Object Animation" as drag sources. Dropping a kind onto
// a timeline track creates the appropriate entity:
//   - Clip: empty clip entity (no media assigned yet; user picks via Properties)
//   - ObjectAnimation: Layer + ObjectAnimationLayer + AnimatedProperties entity
//
// This is the Phase 3.5 entry point for OA layer creation from the UI.
// Media-drop from MediaBin (existing path) is unchanged.
class LayersWindow : public EditorWindow {
public:
    explicit LayersWindow(Engine* engine);
    ~LayersWindow() override = default;

    void render() override;
    const char* getName() const override { return "Layers"; }

private:
    Engine* m_engine{nullptr};
};

} // namespace entity
