#pragma once

#include "EditorWindow.hpp"
#include "../components/AnimatedProperties.hpp"
#include <imgui.h>
#include <functional>
#include <unordered_map>
#include <entt/entt.hpp>

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

    /**
     * Render timeline properties (when no clip selected).
     */
    void renderTimelineProperties();

    /**
     * Render keyframe controls for a property.
     * Shows stopwatch, prev/add/next keyframe buttons.
     * @param property The animatable property to control
     * @param propertyName Display name for the property
     * @param currentValue Current value of the property
     * @param onValueChanged Callback when value is changed via keyframe
     */
    void renderKeyframeControls(AnimatableProperty property, const char* propertyName,
                                float currentValue, std::function<void(float)> onValueChanged = nullptr);

    /**
     * Get the current frame relative to the clip start.
     * Returns -1 if playhead is outside the clip bounds.
     */
    int getCurrentClipFrame() const;

    /**
     * Navigate to the previous keyframe for a property.
     */
    void goToPreviousKeyframe(AnimatableProperty property);

    /**
     * Navigate to the next keyframe for a property.
     */
    void goToNextKeyframe(AnimatableProperty property);

    /**
     * Toggle keyframe at current frame for a property.
     * If keyframe exists, removes it. Otherwise adds one with current value.
     */
    void toggleKeyframeAtCurrentFrame(AnimatableProperty property, float currentValue);

private:
    Timeline* m_timeline{nullptr};  // Non-owning pointer to Timeline

    // Per-entity UI state (prevents static variable leak between clips)
    std::unordered_map<entt::entity, bool> m_uniformScaleState;  // Uniform scale checkbox state per clip
};

} // namespace entity
