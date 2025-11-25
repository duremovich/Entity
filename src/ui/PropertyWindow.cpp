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
#include <imgui.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cmath>
#include <algorithm>

namespace entity {

PropertyWindow::PropertyWindow(Timeline* timeline)
    : m_timeline(timeline)
{
}

void PropertyWindow::render() {
    if (!m_timeline) {
        ImGui::Text("No timeline available");
        return;
    }

    entt::entity selectedClip = m_timeline->getSelectedClip();

    if (selectedClip == entt::null) {
        // No clip selected - show timeline properties
        renderTimelineProperties();
        return;
    }

    auto& registry = m_timeline->getRegistry();

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

    // Position (NDC: -1 to 1 range, center is 0,0)
    ImGui::Text("Position");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Position in NDC (-1 to 1).\n0,0 = center, -1,-1 = bottom-left, 1,1 = top-right");
    }
    ImGui::SetNextItemWidth(-1);
    float pos[3] = { transform->position.x, transform->position.y, transform->position.z };
    if (ImGui::DragFloat3("##position", pos, 0.01f, -2.0f, 2.0f, "%.3f")) {
        transform->setPosition(glm::vec3(pos[0], pos[1], pos[2]));
    }

    // Rotation (already stored in degrees in Transform)
    ImGui::Text("Rotation");
    ImGui::SetNextItemWidth(-1);
    float rot[3] = { transform->rotation.x, transform->rotation.y, transform->rotation.z };
    if (ImGui::DragFloat3("##rotation", rot, 0.5f, -360.0f, 360.0f, "%.1f deg")) {
        transform->setRotation(glm::vec3(rot[0], rot[1], rot[2]));
    }

    // Scale (1.0 = fullscreen, 0.5 = half size)
    ImGui::Text("Scale");
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Scale factor. 1.0 = fullscreen, 0.5 = half size");
    }

    // Uniform scale checkbox (before scale widget so user sees the option)
    static bool uniformScale = true;
    ImGui::Checkbox("Uniform Scale", &uniformScale);

    ImGui::SetNextItemWidth(-1);
    float scale[3] = { transform->scale.x, transform->scale.y, transform->scale.z };
    float prevScale[3] = { scale[0], scale[1], scale[2] };  // Store previous values

    if (ImGui::DragFloat3("##scale", scale, 0.01f, 0.01f, 10.0f, "%.3f")) {
        if (uniformScale) {
            // Find which axis changed and apply ratio to all axes
            float ratio = 1.0f;
            for (int i = 0; i < 3; ++i) {
                if (std::abs(scale[i] - prevScale[i]) > 0.0001f && prevScale[i] > 0.0001f) {
                    ratio = scale[i] / prevScale[i];
                    break;
                }
            }
            // Apply ratio to all axes
            scale[0] = prevScale[0] * ratio;
            scale[1] = prevScale[1] * ratio;
            scale[2] = prevScale[2] * ratio;
            // Clamp to valid range
            for (int i = 0; i < 3; ++i) {
                scale[i] = std::max(0.01f, std::min(10.0f, scale[i]));
            }
        }
        transform->setScale(glm::vec3(scale[0], scale[1], scale[2]));
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

    // Opacity slider
    ImGui::Text("Opacity");
    ImGui::SetNextItemWidth(-1);
    float opacity = layer->opacity;
    if (ImGui::SliderFloat("##opacity", &opacity, 0.0f, 1.0f, "%.2f")) {
        layer->opacity = opacity;
    }

    // Visibility toggle
    ImGui::Checkbox("Visible", &layer->visible);

    // Z-Order (read-only for now, determined by track)
    ImGui::Text("Z-Order: %d", layer->zOrder);
    ImGui::SameLine();
    ImGui::TextDisabled("(determined by track)");

    // Blend mode combo (placeholder for future)
    static const char* blendModes[] = { "Normal", "Add", "Multiply", "Screen" };
    int currentBlendMode = static_cast<int>(layer->blendMode);
    ImGui::Text("Blend Mode");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##blendmode", &currentBlendMode, blendModes, IM_ARRAYSIZE(blendModes))) {
        layer->blendMode = static_cast<BlendMode>(currentBlendMode);
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

} // namespace entity
