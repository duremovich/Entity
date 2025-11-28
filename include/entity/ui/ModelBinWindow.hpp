#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>
#include <string>

namespace entity {

// Forward declarations
class Engine;
class WindowManager;

/**
 * ModelBinWindow - Displays imported 3D models (OBJ files) for use as screens.
 *
 * Shows a list of all models that have been imported into the project.
 * Models can be dragged onto the Screens window to create new screens,
 * or assigned to existing screens.
 */
class ModelBinWindow : public EditorWindow {
public:
    explicit ModelBinWindow(Engine* engine, WindowManager* windowManager);
    ~ModelBinWindow() override = default;

    void render() override;
    const char* getName() const override { return "Model Bin"; }

    /**
     * Import an OBJ file as a model
     */
    void importModel(const std::string& filepath);

private:
    Engine* m_engine{nullptr};
    WindowManager* m_windowManager{nullptr};
};

} // namespace entity
