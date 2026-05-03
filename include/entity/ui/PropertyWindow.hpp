#pragma once

#include "EditorWindow.hpp"
#include "../components/AnimatedProperties.hpp"
#include "../core/Types.hpp"
#include <imgui.h>
#include <functional>
#include <optional>
#include <unordered_map>
#include <entt/entt.hpp>

namespace entity {

// Forward declarations
class Timeline;
class CommandDispatcher;

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

    // Optional — when set, UI edits go through the dispatcher as
    // undoable commands. Without it, edits still mutate live state but
    // aren't recorded on the undo stack (fallback path for headless tests
    // that construct PropertyWindow without an Engine).
    void setCommandDispatcher(CommandDispatcher* dispatcher) { m_dispatcher = dispatcher; }

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
     * Render playback mode settings.
     */
    void renderPlaybackSection();

    /**
     * Render clip info (read-only).
     */
    void renderClipInfo();

    /**
     * Render timeline properties (when no clip selected).
     */
    void renderTimelineProperties();

    /**
     * Render screen properties (when a screen is selected).
     */
    void renderScreenProperties();

    /**
     * Render projector properties (when a projector is selected).
     */
    void renderProjectorProperties();

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

    /**
     * Update keyframe value when property is changed via UI.
     * If property has animation (keyframes), auto-adds/updates keyframe at current frame.
     * This enables After Effects-style workflow where changing an animated property
     * automatically creates/updates keyframes.
     * @param property The animatable property being changed
     * @param newValue The new value to set
     */
    void updateKeyframeOnValueChange(AnimatableProperty property, float newValue);

private:
    Timeline* m_timeline{nullptr};  // Non-owning pointer to Timeline
    CommandDispatcher* m_dispatcher{nullptr};  // Non-owning, optional

    // Per-entity UI state (prevents static variable leak between clips)
    std::unordered_map<entt::entity, bool> m_uniformScaleState;  // Uniform scale checkbox state per clip

    // Pre-edit snapshot captured on IsItemActivated. If the property was
    // keyframed at drag start AND the playhead is inside the clip,
    // updateKeyframeOnValueChange() rewrites the keyframe at that clip
    // frame during the drag. In that case the undoable unit is the
    // keyframe (restore-or-delete), not the scalar — restoring the scalar
    // alone does nothing because AnimationSystem will overwrite it from
    // the keyframe track on the next tick.
    struct PreEditState {
        float scalarValue{0.0f};
        bool wasKeyframed{false};            // emit UpsertKeyframe vs scalar command
        FrameNumber keyframeFrame{0};        // clip-local frame of the keyframe write
        std::optional<float> keyframeValue;  // nullopt = no keyframe existed there
    };
    PreEditState m_preEditOpacity;
    PreEditState m_preEditRotZ;
};

} // namespace entity
