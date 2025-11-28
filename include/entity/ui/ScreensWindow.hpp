#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>
#include <entt/entt.hpp>

namespace entity {

// Forward declaration
class Engine;

/**
 * ScreensWindow - Lists and manages screens (projection surfaces) in the scene.
 *
 * Provides a list view of screens. Selecting a screen shows its properties
 * in the Properties window. Properties editing is handled by PropertyWindow.
 */
class ScreensWindow : public EditorWindow {
public:
    explicit ScreensWindow(Engine* engine);
    ~ScreensWindow() override = default;

    void render() override;
    const char* getName() const override { return "Screens"; }

    /**
     * Create a new screen with default settings
     */
    entt::entity createScreen(const char* name = "New Screen");

private:
    Engine* m_engine{nullptr};
};

} // namespace entity
