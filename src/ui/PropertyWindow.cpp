/**
 * PropertyWindow Implementation
 *
 * Displays and allows editing of selected clip properties.
 */

#include "entity/ui/PropertyWindow.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/core/Types.hpp"
#include "entity/components/Transform.hpp"
#include "entity/components/MediaLayer.hpp"
#include "entity/components/Clip.hpp"
#include "entity/components/AnimatedProperties.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/TimelineTrack.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/command/Commands.hpp"
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <optional>
#include <utility>
#include <vector>

namespace entity {

namespace {

// Map a clip entity back to (trackIndex, clipIndex) — the address commands
// use. Returns nullopt if the clip isn't actually on any track.
std::optional<std::pair<int, int>>
findClipIndices(Timeline* timeline, entt::entity clipEntity) {
    if (!timeline || clipEntity == entt::null) return std::nullopt;
    auto& registry = timeline->getRegistry();
    const auto& tracks = timeline->getTracks();
    for (size_t ti = 0; ti < tracks.size(); ++ti) {
        auto* track = registry.try_get<TimelineTrack>(tracks[ti]);
        if (!track) continue;
        for (size_t ci = 0; ci < track->clips.size(); ++ci) {
            if (track->clips[ci] == clipEntity) {
                return std::make_pair(static_cast<int>(ti), static_cast<int>(ci));
            }
        }
    }
    return std::nullopt;
}

} // namespace

PropertyWindow::PropertyWindow(Timeline* timeline)
    : m_timeline(timeline)
{
}

void PropertyWindow::render() {
    if (!m_timeline) {
        ImGui::Text("No timeline available");
        return;
    }

    auto& registry = m_timeline->getRegistry();

    // Check for selected screen first
    entt::entity selectedScreen = m_timeline->getSelectedScreen();
    if (selectedScreen != entt::null && registry.valid(selectedScreen)) {
        renderScreenProperties();
        return;
    }

    // Check for selected clip
    entt::entity selectedClip = m_timeline->getSelectedClip();

    if (selectedClip == entt::null) {
        // No clip selected - show timeline properties
        renderTimelineProperties();
        return;
    }

    if (!registry.valid(selectedClip)) {
        ImGui::TextDisabled("Invalid selection");
        return;
    }

    // CRITICAL: Push unique ID for this clip to prevent ImGui widget state collision
    // Without this, all clips share the same widget IDs and state bleeds between them
    ImGui::PushID(static_cast<int>(selectedClip));

    // Show clip name at top
    const auto* clip = registry.try_get<Clip>(selectedClip);
    if (clip) {
        // Extract filename from path
        size_t lastSlash = clip->filepath.find_last_of("/\\");
        std::string filename = (lastSlash != std::string::npos)
            ? clip->filepath.substr(lastSlash + 1)
            : clip->filepath;
        ImGui::Text("Clip: %s", filename.c_str());
        ImGui::Separator();
    }

    // Transform section
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderTransformSection();
    }

    // Layer/Opacity section
    if (ImGui::CollapsingHeader("Layer", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderLayerSection();
    }

    // Playback section
    if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
        renderPlaybackSection();
    }

    // Clip Info section
    if (ImGui::CollapsingHeader("Clip Info")) {
        renderClipInfo();
    }

    // Pop the clip-specific ID
    ImGui::PopID();
}

void PropertyWindow::renderTransformSection() {
    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    auto* transform = registry.try_get<Transform>(selectedClip);

    if (!transform) {
        ImGui::TextDisabled("No transform component");
        return;
    }

    // Position X with keyframe controls
    renderKeyframeControls(AnimatableProperty::PositionX, "Position X", transform->position.x);
    ImGui::SameLine();
    ImGui::Text("Position X");
    ImGui::SetNextItemWidth(-1);
    float posX = transform->position.x;
    if (ImGui::DragFloat("##posX", &posX, 0.01f, -2.0f, 2.0f, "%.3f")) {
        transform->setPosition(glm::vec3(posX, transform->position.y, transform->position.z));
        updateKeyframeOnValueChange(AnimatableProperty::PositionX, posX);
    }

    // Position Y with keyframe controls
    renderKeyframeControls(AnimatableProperty::PositionY, "Position Y", transform->position.y);
    ImGui::SameLine();
    ImGui::Text("Position Y");
    ImGui::SetNextItemWidth(-1);
    float posY = transform->position.y;
    if (ImGui::DragFloat("##posY", &posY, 0.01f, -2.0f, 2.0f, "%.3f")) {
        transform->setPosition(glm::vec3(transform->position.x, posY, transform->position.z));
        updateKeyframeOnValueChange(AnimatableProperty::PositionY, posY);
    }

    ImGui::Spacing();

    // Rotation Z with keyframe controls
    renderKeyframeControls(AnimatableProperty::Rotation, "Rotation", transform->rotation.z);
    ImGui::SameLine();
    ImGui::Text("Rotation");
    ImGui::SetNextItemWidth(-1);
    float rotZ = transform->rotation.z;
    bool rotChanged = ImGui::DragFloat("##rotZ", &rotZ, 0.5f, -360.0f, 360.0f, "%.1f deg");
    if (ImGui::IsItemActivated()) {
        m_preEditRotZ = transform->rotation.z;
    }
    if (rotChanged) {
        transform->setRotation(glm::vec3(transform->rotation.x, transform->rotation.y, rotZ));
        updateKeyframeOnValueChange(AnimatableProperty::Rotation, rotZ);
    }
    if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
        if (auto idx = findClipIndices(m_timeline, selectedClip)) {
            auto cmd = std::make_unique<SetClipRotationCommand>(
                idx->first, idx->second,
                transform->rotation.x, transform->rotation.y, transform->rotation.z);
            cmd->setPreviousRotation(transform->rotation.x, transform->rotation.y, m_preEditRotZ);
            m_dispatcher->enqueue(std::move(cmd));
        }
    }

    ImGui::Spacing();

    // Scale section
    ImGui::Text("Scale");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Scale factor. 1.0 = fullscreen, 0.5 = half size");
    }

    // Uniform scale checkbox - use per-entity state map to prevent static variable leak
    auto [it, inserted] = m_uniformScaleState.try_emplace(selectedClip, true);
    bool& uniformScale = it->second;
    ImGui::Checkbox("Uniform Scale", &uniformScale);

    // Scale X with keyframe controls
    renderKeyframeControls(AnimatableProperty::ScaleX, "Scale X", transform->scale.x);
    ImGui::SameLine();
    ImGui::Text("Scale X");
    ImGui::SetNextItemWidth(-1);
    float scaleX = transform->scale.x;
    float prevScaleX = scaleX;
    if (ImGui::DragFloat("##scaleX", &scaleX, 0.01f, 0.01f, 10.0f, "%.3f")) {
        if (uniformScale && prevScaleX > 0.0001f) {
            float ratio = scaleX / prevScaleX;
            float newScaleY = transform->scale.y * ratio;
            transform->setScale(glm::vec3(scaleX, newScaleY, transform->scale.z * ratio));
            updateKeyframeOnValueChange(AnimatableProperty::ScaleX, scaleX);
            updateKeyframeOnValueChange(AnimatableProperty::ScaleY, newScaleY);
        } else {
            transform->setScale(glm::vec3(scaleX, transform->scale.y, transform->scale.z));
            updateKeyframeOnValueChange(AnimatableProperty::ScaleX, scaleX);
        }
    }

    // Scale Y with keyframe controls
    renderKeyframeControls(AnimatableProperty::ScaleY, "Scale Y", transform->scale.y);
    ImGui::SameLine();
    ImGui::Text("Scale Y");
    ImGui::SetNextItemWidth(-1);
    float scaleY = transform->scale.y;
    float prevScaleY = scaleY;
    if (ImGui::DragFloat("##scaleY", &scaleY, 0.01f, 0.01f, 10.0f, "%.3f")) {
        if (uniformScale && prevScaleY > 0.0001f) {
            float ratio = scaleY / prevScaleY;
            float newScaleX = transform->scale.x * ratio;
            transform->setScale(glm::vec3(newScaleX, scaleY, transform->scale.z * ratio));
            updateKeyframeOnValueChange(AnimatableProperty::ScaleX, newScaleX);
            updateKeyframeOnValueChange(AnimatableProperty::ScaleY, scaleY);
        } else {
            transform->setScale(glm::vec3(transform->scale.x, scaleY, transform->scale.z));
            updateKeyframeOnValueChange(AnimatableProperty::ScaleY, scaleY);
        }
    }

    ImGui::Spacing();

    // Reset button
    if (ImGui::Button("Reset Transform")) {
        transform->setPosition(glm::vec3(0.0f));
        transform->setRotation(glm::vec3(0.0f));
        transform->setScale(glm::vec3(1.0f));
    }
}

void PropertyWindow::renderLayerSection() {
    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    auto* layer = registry.try_get<MediaLayer>(selectedClip);

    if (!layer) {
        ImGui::TextDisabled("No layer component");
        return;
    }

    // Opacity with keyframe controls
    renderKeyframeControls(AnimatableProperty::Opacity, "Opacity", layer->opacity);
    ImGui::SameLine();
    ImGui::Text("Opacity");
    ImGui::SetNextItemWidth(-1);
    float opacity = layer->opacity;
    bool opacityChanged = ImGui::SliderFloat("##opacity", &opacity, 0.0f, 1.0f, "%.2f");
    if (ImGui::IsItemActivated()) {
        m_preEditOpacity = layer->opacity;
    }
    if (opacityChanged) {
        layer->opacity = opacity;
        updateKeyframeOnValueChange(AnimatableProperty::Opacity, opacity);
    }
    // Emit the undoable command on drag end so a single drag = single undo
    // entry. Live state has already been mutated above for responsiveness;
    // the command's execute() is effectively a no-op on current state but
    // stores m_previousOpacity = m_preEditOpacity for undo.
    if (ImGui::IsItemDeactivatedAfterEdit() && m_dispatcher) {
        if (auto idx = findClipIndices(m_timeline, selectedClip)) {
            auto cmd = std::make_unique<SetClipOpacityCommand>(idx->first, idx->second, layer->opacity);
            cmd->setPreviousOpacity(m_preEditOpacity);
            m_dispatcher->enqueue(std::move(cmd));
        }
    }

    // Visibility toggle
    ImGui::Checkbox("Visible", &layer->visible);

    // Z-Order (read-only for now, determined by track)
    ImGui::Text("Z-Order: %d", layer->zOrder);
    ImGui::SameLine();
    ImGui::TextDisabled("(determined by track)");

    // Blend mode combo. Order must match the BlendMode enum in
    // include/entity/core/Types.hpp; the combo index is static_cast'd straight
    // to BlendMode.
    static const char* blendModes[] = {
        "Normal", "Add", "Multiply", "Screen", "Overlay",
        "Soft Light", "Hard Light", "Color Dodge", "Color Burn",
        "Darken", "Lighten", "Difference", "Exclusion"
    };
    int currentBlendMode = static_cast<int>(layer->blendMode);
    ImGui::Text("Blend Mode");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##blendmode", &currentBlendMode, blendModes, IM_ARRAYSIZE(blendModes))) {
        BlendMode prev = layer->blendMode;
        BlendMode next = static_cast<BlendMode>(currentBlendMode);
        layer->blendMode = next;
        if (m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, selectedClip)) {
                auto cmd = std::make_unique<SetClipBlendModeCommand>(idx->first, idx->second, next);
                cmd->setPreviousMode(prev);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }
    }
}

void PropertyWindow::renderPlaybackSection() {
    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    auto* clip = registry.try_get<Clip>(selectedClip);

    if (!clip) {
        ImGui::TextDisabled("No clip component");
        return;
    }

    // Playback mode dropdown
    ImGui::Text("Extend Mode");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Determines what happens when clip is extended\nbeyond its source media duration.\n\n"
                          "Freeze: Hold on last frame\n"
                          "Loop: Restart from beginning\n"
                          "Ping-Pong: Play forward then backward");
    }

    static const char* playbackModes[] = { "Freeze", "Loop", "Ping-Pong" };
    int currentMode = static_cast<int>(clip->playbackMode);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##playbackMode", &currentMode, playbackModes, IM_ARRAYSIZE(playbackModes))) {
        PlaybackMode prev = clip->playbackMode;
        PlaybackMode next = static_cast<PlaybackMode>(currentMode);
        clip->playbackMode = next;
        if (m_dispatcher) {
            if (auto idx = findClipIndices(m_timeline, selectedClip)) {
                auto cmd = std::make_unique<SetClipPlaybackModeCommand>(idx->first, idx->second, next);
                cmd->setPreviousMode(prev);
                m_dispatcher->enqueue(std::move(cmd));
            }
        }
    }

    // Screen mapping (which screen this clip renders to)
    ImGui::Spacing();
    ImGui::Text("Target Screen");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Select which screen this clip renders to.\n"
                          "Default renders to all screens.");
    }

    // Build list of available screens
    auto screenView = registry.view<Screen>();
    std::vector<entt::entity> screens;
    std::vector<const char*> screenNames;
    screenNames.push_back("Default (All)");
    screens.push_back(entt::null);

    for (auto screenEntity : screenView) {
        const Screen& screen = screenView.get<Screen>(screenEntity);
        screens.push_back(screenEntity);
        screenNames.push_back(screen.name.c_str());
    }

    // Find current selection index
    int currentScreenIdx = 0;
    for (size_t i = 0; i < screens.size(); ++i) {
        if (screens[i] == clip->targetScreen) {
            currentScreenIdx = static_cast<int>(i);
            break;
        }
    }

    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##targetScreen", &currentScreenIdx, screenNames.data(), static_cast<int>(screenNames.size()))) {
        clip->targetScreen = screens[currentScreenIdx];
        std::cout << "[PropertyWindow] Target screen changed to: "
                  << (clip->targetScreen == entt::null ? "ALL" : std::to_string(static_cast<uint32_t>(clip->targetScreen)))
                  << " (" << screenNames[currentScreenIdx] << ")" << std::endl;
    }

    // Show source vs timeline duration info
    ImGui::Spacing();
    ImGui::Text("Source Frames: %d", clip->totalMediaFrames);
    ImGui::Text("Timeline Frames: %d", clip->duration);

    // Show if clip is extended
    if (clip->duration > clip->totalMediaFrames && clip->totalMediaFrames > 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Clip extended beyond source");
        FrameNumber extraFrames = clip->duration - clip->totalMediaFrames;
        ImGui::Text("Extra frames: %d", extraFrames);
    }
}

void PropertyWindow::renderClipInfo() {
    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    const auto* clip = registry.try_get<Clip>(selectedClip);

    if (!clip) {
        ImGui::TextDisabled("No clip component");
        return;
    }

    // File path
    ImGui::Text("File:");
    ImGui::TextWrapped("%s", clip->filepath.c_str());

    ImGui::Separator();

    // Timing info
    ImGui::Text("Start Frame: %d", clip->startFrame);
    ImGui::Text("Duration: %d frames", clip->duration);
    ImGui::Text("Media Start: %d", clip->mediaStartFrame);
    ImGui::Text("Frame Rate: %.2f fps", clip->framerate);

    // Calculate duration in seconds
    float durationSec = static_cast<float>(clip->duration) / static_cast<float>(clip->framerate);
    ImGui::Text("Duration: %.2f seconds", durationSec);

    ImGui::Separator();

    // Media info
    ImGui::Text("Resolution: %dx%d", clip->width, clip->height);
    ImGui::Text("Has Alpha: %s", clip->hasAlpha ? "Yes" : "No");

    // Media type (use helper function from Types.hpp)
    ImGui::Text("Media Type: %s", MediaTypeToString(clip->mediaType));
}

void PropertyWindow::renderTimelineProperties() {
    ImGui::Text("Timeline Properties");
    ImGui::Separator();
    ImGui::Spacing();

    // Duration editor
    ImGui::Text("Duration");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Total timeline duration in minutes.\nClips can extend this automatically if placed beyond the end.");
    }

    // Convert microseconds to minutes for editing
    Timecode currentDuration = m_timeline->getDuration();
    float durationMinutes = static_cast<float>(currentDuration) / 60000000.0f;  // 60 * 1,000,000

    ImGui::SetNextItemWidth(100);
    if (ImGui::DragFloat("##duration_minutes", &durationMinutes, 0.1f, 0.5f, 120.0f, "%.1f min")) {
        // Convert back to microseconds
        Timecode newDuration = static_cast<Timecode>(durationMinutes * 60000000.0);
        m_timeline->setDuration(newDuration);
    }

    // Show duration in different formats
    double durationSeconds = currentDuration / 1000000.0;
    int hours = static_cast<int>(durationSeconds) / 3600;
    int minutes = (static_cast<int>(durationSeconds) % 3600) / 60;
    int seconds = static_cast<int>(durationSeconds) % 60;
    ImGui::Text("(%02d:%02d:%02d)", hours, minutes, seconds);

    ImGui::Spacing();

    // Quick presets
    ImGui::Text("Presets:");
    if (ImGui::Button("1 min")) {
        m_timeline->setDuration(60000000);
    }
    ImGui::SameLine();
    if (ImGui::Button("5 min")) {
        m_timeline->setDuration(300000000);
    }
    ImGui::SameLine();
    if (ImGui::Button("10 min")) {
        m_timeline->setDuration(600000000);
    }
    ImGui::SameLine();
    if (ImGui::Button("30 min")) {
        m_timeline->setDuration(1800000000);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Frame rate (read-only for now)
    ImGui::Text("Frame Rate: %.2f fps", m_timeline->getFrameRate());

    // Track count
    ImGui::Text("Tracks: %zu", m_timeline->getTrackCount());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Select a clip to edit its properties");
}

int PropertyWindow::getCurrentClipFrame() const {
    if (!m_timeline) return -1;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return -1;

    auto& registry = m_timeline->getRegistry();
    const auto* clip = registry.try_get<Clip>(selectedClip);
    if (!clip) return -1;

    // Get current timeline frame
    Timecode currentTime = m_timeline->getCurrentTime();
    FrameNumber currentFrame = static_cast<FrameNumber>(
        (currentTime / 1000000.0) * m_timeline->getFrameRate());

    // Convert to clip-relative frame
    FrameNumber clipFrame = currentFrame - clip->startFrame;

    // Check if playhead is within clip bounds
    if (clipFrame < 0 || clipFrame >= clip->duration) {
        return -1;  // Playhead outside clip
    }

    return static_cast<int>(clipFrame);
}

void PropertyWindow::goToPreviousKeyframe(AnimatableProperty property) {
    if (!m_timeline) return;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    const auto* animProps = registry.try_get<AnimatedProperties>(selectedClip);
    const auto* clip = registry.try_get<Clip>(selectedClip);
    if (!animProps || !clip) return;

    const KeyframeTrack* track = animProps->getTrack(property);
    if (!track || track->keyframes.empty()) return;

    // Get current clip frame
    int currentClipFrame = getCurrentClipFrame();
    if (currentClipFrame < 0) {
        // If outside clip, go to last keyframe
        FrameNumber lastKfFrame = track->keyframes.back().frame;
        FrameNumber timelineFrame = clip->startFrame + lastKfFrame;
        Timecode targetTime = static_cast<Timecode>(
            (timelineFrame / m_timeline->getFrameRate()) * 1000000.0);
        m_timeline->seek(targetTime);
        return;
    }

    // Find previous keyframe
    FrameNumber prevFrame = -1;
    for (const auto& kf : track->keyframes) {
        if (kf.frame < currentClipFrame) {
            prevFrame = kf.frame;
        } else {
            break;  // Keyframes are sorted, stop when we pass current
        }
    }

    if (prevFrame >= 0) {
        FrameNumber timelineFrame = clip->startFrame + prevFrame;
        Timecode targetTime = static_cast<Timecode>(
            (timelineFrame / m_timeline->getFrameRate()) * 1000000.0);
        m_timeline->seek(targetTime);
    }
}

void PropertyWindow::goToNextKeyframe(AnimatableProperty property) {
    if (!m_timeline) return;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    const auto* animProps = registry.try_get<AnimatedProperties>(selectedClip);
    const auto* clip = registry.try_get<Clip>(selectedClip);
    if (!animProps || !clip) return;

    const KeyframeTrack* track = animProps->getTrack(property);
    if (!track || track->keyframes.empty()) return;

    // Get current clip frame
    int currentClipFrame = getCurrentClipFrame();
    if (currentClipFrame < 0) {
        // If outside clip, go to first keyframe
        FrameNumber firstKfFrame = track->keyframes.front().frame;
        FrameNumber timelineFrame = clip->startFrame + firstKfFrame;
        Timecode targetTime = static_cast<Timecode>(
            (timelineFrame / m_timeline->getFrameRate()) * 1000000.0);
        m_timeline->seek(targetTime);
        return;
    }

    // Find next keyframe
    for (const auto& kf : track->keyframes) {
        if (kf.frame > currentClipFrame) {
            FrameNumber timelineFrame = clip->startFrame + kf.frame;
            Timecode targetTime = static_cast<Timecode>(
                (timelineFrame / m_timeline->getFrameRate()) * 1000000.0);
            m_timeline->seek(targetTime);
            return;
        }
    }
}

void PropertyWindow::toggleKeyframeAtCurrentFrame(AnimatableProperty property, float currentValue) {
    if (!m_timeline) return;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    int clipFrame = getCurrentClipFrame();
    if (clipFrame < 0) return;  // Playhead not on clip

    auto& registry = m_timeline->getRegistry();

    // Get or create AnimatedProperties component
    auto* animProps = registry.try_get<AnimatedProperties>(selectedClip);
    if (!animProps) {
        registry.emplace<AnimatedProperties>(selectedClip);
        animProps = registry.try_get<AnimatedProperties>(selectedClip);
    }

    KeyframeTrack& track = animProps->getOrCreateTrack(property);

    // Check if keyframe exists at this frame
    const Keyframe* existingKf = track.getKeyframeAt(static_cast<FrameNumber>(clipFrame));
    if (existingKf) {
        // Remove existing keyframe
        track.removeKeyframe(static_cast<FrameNumber>(clipFrame));
    } else {
        // Add new keyframe with current value
        track.addKeyframe(static_cast<FrameNumber>(clipFrame), currentValue);
    }
}

void PropertyWindow::updateKeyframeOnValueChange(AnimatableProperty property, float newValue) {
    if (!m_timeline) return;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    int clipFrame = getCurrentClipFrame();
    if (clipFrame < 0) return;  // Playhead not on clip

    auto& registry = m_timeline->getRegistry();

    // Check if property has animation (keyframes)
    auto* animProps = registry.try_get<AnimatedProperties>(selectedClip);
    if (!animProps) return;

    const KeyframeTrack* track = animProps->getTrack(property);
    if (!track || !track->hasKeyframes()) return;

    // Property has keyframes - auto-update/add keyframe at current frame
    KeyframeTrack& mutableTrack = animProps->getOrCreateTrack(property);
    mutableTrack.addKeyframe(static_cast<FrameNumber>(clipFrame), newValue);
}

void PropertyWindow::renderKeyframeControls(AnimatableProperty property, const char* propertyName,
                                            float currentValue, std::function<void(float)> onValueChanged) {
    if (!m_timeline) return;

    entt::entity selectedClip = m_timeline->getSelectedClip();
    if (selectedClip == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    const auto* animProps = registry.try_get<AnimatedProperties>(selectedClip);
    const KeyframeTrack* track = animProps ? animProps->getTrack(property) : nullptr;

    bool hasKeyframes = track && track->hasKeyframes();
    int clipFrame = getCurrentClipFrame();
    bool hasKeyframeAtCurrentFrame = false;

    if (track && clipFrame >= 0) {
        hasKeyframeAtCurrentFrame = (track->getKeyframeAt(static_cast<FrameNumber>(clipFrame)) != nullptr);
    }

    // Push unique ID for this property's controls
    ImGui::PushID(static_cast<int>(property));

    // Stopwatch button (filled if has keyframes)
    ImU32 stopwatchColor = hasKeyframes ? IM_COL32(255, 180, 50, 255) : IM_COL32(128, 128, 128, 255);
    ImGui::PushStyleColor(ImGuiCol_Text, stopwatchColor);
    if (ImGui::SmallButton(hasKeyframes ? "(*)" : "( )")) {
        // Toggle animation - if no keyframes, add one at current frame
        // If has keyframes, we could clear them (but let's just add at current frame for now)
        if (clipFrame >= 0) {
            toggleKeyframeAtCurrentFrame(property, currentValue);
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(hasKeyframes ? "Animation enabled - click to toggle keyframe at current frame"
                                       : "Click to add keyframe at current frame");
    }

    ImGui::SameLine();

    // Previous keyframe button
    bool canGoPrev = hasKeyframes && clipFrame > 0;
    if (!canGoPrev) ImGui::BeginDisabled();
    if (ImGui::SmallButton("<")) {
        goToPreviousKeyframe(property);
    }
    if (!canGoPrev) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Go to previous keyframe");
    }

    ImGui::SameLine();

    // Add/remove keyframe diamond button
    ImU32 diamondColor = hasKeyframeAtCurrentFrame ? IM_COL32(255, 180, 50, 255) : IM_COL32(128, 128, 128, 255);
    ImGui::PushStyleColor(ImGuiCol_Text, diamondColor);
    if (ImGui::SmallButton(hasKeyframeAtCurrentFrame ? "<>" : "<>")) {
        if (clipFrame >= 0) {
            toggleKeyframeAtCurrentFrame(property, currentValue);
        }
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(hasKeyframeAtCurrentFrame ? "Remove keyframe at current frame"
                                                    : "Add keyframe at current frame");
    }

    ImGui::SameLine();

    // Next keyframe button
    bool canGoNext = hasKeyframes;
    if (!canGoNext) ImGui::BeginDisabled();
    if (ImGui::SmallButton(">")) {
        goToNextKeyframe(property);
    }
    if (!canGoNext) ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Go to next keyframe");
    }

    ImGui::PopID();
}

void PropertyWindow::renderScreenProperties() {
    if (!m_timeline) return;

    entt::entity selectedScreen = m_timeline->getSelectedScreen();
    if (selectedScreen == entt::null) return;

    auto& registry = m_timeline->getRegistry();
    Screen* screen = registry.try_get<Screen>(selectedScreen);

    if (!screen) {
        ImGui::TextDisabled("Invalid screen");
        return;
    }

    // Push unique ID for this screen
    ImGui::PushID(static_cast<int>(selectedScreen));

    ImGui::Text("Screen: %s", screen->name.c_str());
    ImGui::Separator();

    // Name
    if (ImGui::CollapsingHeader("Identity", ImGuiTreeNodeFlags_DefaultOpen)) {
        char nameBuf[256];
        strncpy(nameBuf, screen->name.c_str(), sizeof(nameBuf) - 1);
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            screen->name = nameBuf;
        }
    }

    // Geometry
    if (ImGui::CollapsingHeader("Geometry", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (screen->modelEntity != entt::null && registry.valid(screen->modelEntity)) {
            Model* model = registry.try_get<Model>(screen->modelEntity);
            if (model) {
                ImGui::Text("Model: %s", model->name.c_str());
                ImGui::Text("Vertices: %zu", model->mesh.vertices.size());
                ImGui::Text("Triangles: %zu", model->mesh.indices.size() / 3);
                if (ImGui::SmallButton("Clear Model")) {
                    screen->modelEntity = entt::null;
                }
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f), "Invalid model reference");
                screen->modelEntity = entt::null;
            }
        } else {
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No model assigned");
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Drag a model from Model Bin");

            // Accept model drops
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODEL_ENTITY")) {
                    uint32_t modelEntityId = *static_cast<const uint32_t*>(payload->Data);
                    screen->modelEntity = static_cast<entt::entity>(modelEntityId);
                }
                ImGui::EndDragDropTarget();
            }
        }
    }

    // Resolution
    if (ImGui::CollapsingHeader("Resolution", ImGuiTreeNodeFlags_DefaultOpen)) {
        int resolution[2] = {static_cast<int>(screen->width), static_cast<int>(screen->height)};
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt2("Size", resolution)) {
            screen->width = static_cast<uint32_t>(std::max(1, resolution[0]));
            screen->height = static_cast<uint32_t>(std::max(1, resolution[1]));
        }

        // Resolution presets
        if (ImGui::Button("1920x1080")) { screen->width = 1920; screen->height = 1080; }
        ImGui::SameLine();
        if (ImGui::Button("3840x2160")) { screen->width = 3840; screen->height = 2160; }
        ImGui::SameLine();
        if (ImGui::Button("1080x1920")) { screen->width = 1080; screen->height = 1920; }
    }

    // Transform
    if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Position");
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat3("##pos", screen->position.data(), 0.1f);

        ImGui::Text("Rotation");
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat3("##rot", screen->rotation.data(), 1.0f, -180.0f, 180.0f);

        ImGui::Text("Scale");
        ImGui::SetNextItemWidth(-1);
        ImGui::DragFloat3("##scale", screen->scale.data(), 0.01f, 0.01f, 100.0f);

        if (ImGui::Button("Reset Transform")) {
            screen->position = {0.0f, 0.0f, 0.0f};
            screen->rotation = {0.0f, 0.0f, 0.0f};
            screen->scale = {1.0f, 1.0f, 1.0f};
        }
    }

    // Display
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Visible", &screen->visible);
        ImGui::SetNextItemWidth(-1);
        ImGui::SliderFloat("Opacity", &screen->opacity, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(-1);
        ImGui::DragInt("Z-Order", &screen->zOrder);
    }

    ImGui::PopID();
}

} // namespace entity
