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
namespace effects { class EffectKindRegistry; }

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

    // Optional — when set, the Effects section lists registered kinds in
    // the "Add Effect" menu (issue #54). Without it, the section degrades
    // to a read-only listing of effects already on the clip.
    void setEffectKindRegistry(const effects::EffectKindRegistry* reg) {
        m_effectKindRegistry = reg;
    }

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
     * Render prop properties (when a prop is selected). Shape mirrors
     * renderScreenProperties() minus the resolution / type controls —
     * props don't render to a compose target.
     */
    void renderPropProperties();

    /**
     * Render projector properties (when a projector is selected).
     */
    void renderProjectorProperties();

    /**
     * Render cue properties (when a cue is selected via Timeline::setSelectedCueNumber).
     * Phase A — number, label, timestamp editable; each edit enqueues
     * EditCueCommand with setPreviousState capturing the pre-edit cue.
     */
    void renderCueProperties();

    /**
     * Render section break properties (when a break is selected via
     * Timeline::setSelectedSectionBreak). Color, frame, and fadeSeconds
     * are editable.
     */
    void renderSectionBreakProperties();

    /**
     * Render OA layer properties: target picker + keyframe editor.
     * Called when the selected entity has ObjectAnimationLayer but no Clip.
     */
    void renderOALayerProperties(entt::entity entity);

    /**
     * Render Generative layer properties: identity (name/color), target
     * screen picker, opacity, render-target size. Called when the selected
     * entity has GenerativeLayer but no Clip. Sub-kind (Muncher, future
     * particles, ...) is detected via composition — extra widgets get
     * folded in here per state component.
     */
    void renderGenerativeLayerProperties(entt::entity entity);

    /**
     * Render the per-layer effects section (issue #54). Lists the current
     * EffectChain entries with parameter sliders + enable/remove. Shows an
     * "Add Effect" popup populated from the EffectKindRegistry.
     */
    void renderEffectsSection(entt::entity layerEntity);

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
     * Get the current frame relative to an OA layer's startFrame.
     * Returns -1 if the playhead is outside [startFrame, startFrame+duration).
     */
    int getCurrentOAFrame(entt::entity oaEntity) const;

    /**
     * Render interactive keyframe controls for an OA layer property.
     * Shows stopwatch toggle, prev/add-at-current/next buttons. Uses
     * getCurrentOAFrame instead of getCurrentClipFrame.
     */
    void renderOAKeyframeControls(entt::entity oaEntity,
                                  AnimatableProperty property,
                                  const char* propertyName);

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
    const effects::EffectKindRegistry* m_effectKindRegistry{nullptr};  // Non-owning, optional

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

    // Simple scalar pre-edit captures for the In/Out point DragInts in
    // renderClipInfo. mediaStartFrame, mediaOutFrame, and duration aren't
    // animatable properties so the keyframe-aware PreEditState shape is
    // overkill.
    FrameNumber m_preEditMediaStartFrame{0};
    FrameNumber m_preEditMediaOutFrame{0};
    FrameNumber m_preEditDuration{0};

    // Pre-edit capture for the active Effect-parameter slider (only one
    // slider is active in ImGui at a time, so a single scalar is enough).
    float m_preEditEffectFloat{0.0f};
};

} // namespace entity
