#pragma once

#include "EditorWindow.hpp"
#include <imgui.h>

namespace entity {

// Forward declarations
class Timeline;

/**
 * PropertyWindow - Displays and edits properties of the selected clip.
 *
 * Shows:
 * - Transform properties (position, rotation, scale)
 * - Opacity
 * - Clip info (duration, filepath)
 */
class PropertyWindow : public EditorWindow {
public:
    explicit PropertyWindow(Timeline* timeline);
    ~PropertyWindow() override = default;

    void render() override;
    const char* getName() const override { return "Properties"; }

    ImGuiWindowFlags getWindowFlags() const override {
        return ImGuiWindowFlags_None;
    }

private:
    /**
     * Render transform property editors.
     */
    void renderTransformSection();

    /**
     * Render layer/opacity property editors.
     */
    void renderLayerSection();

    /**
     * Render clip info (read-only).
     */
    void renderClipInfo();

private:
    Timeline* m_timeline{nullptr};  // Non-owning pointer to Timeline
};

} // namespace entity
