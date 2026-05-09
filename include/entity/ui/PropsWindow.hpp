#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>
#include <entt/entt.hpp>

namespace entity {

class Engine;

/**
 * PropsWindow — Lists and manages stage props (non-screen meshes).
 *
 * Props are pre-visualization-only geometry: set pieces, venue
 * architecture, etc. They render in the 3D stage view alongside screens
 * but never appear in projector output. See Prop.hpp for the data
 * model and CLAUDE.md / docs/adr/0014 for why they stay editor-only.
 *
 * UX mirrors ScreensWindow: "+ Add Prop" button, list of props with
 * selection, drag-drop a Model from ModelBin to assign geometry. The
 * actual property editor (name, transform, model picker, color) lives
 * in PropertyWindow::renderPropProperties.
 */
class PropsWindow : public EditorWindow {
public:
    explicit PropsWindow(Engine* engine);
    ~PropsWindow() override = default;

    void render() override;
    const char* getName() const override { return "Props"; }

    /**
     * Create a new prop with default settings. If any Models exist,
     * auto-assigns the first one (mirrors ScreensWindow::createScreen).
     */
    entt::entity createProp(const char* name = "Prop");

private:
    Engine* m_engine{nullptr};
};

} // namespace entity
