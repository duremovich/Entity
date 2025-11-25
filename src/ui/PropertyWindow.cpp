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
        ImGui::TextDisabled("No clip selected");
        ImGui::Spacing();
        ImGui::TextWrapped("Select a clip in the timeline to edit its properties.");
        return;
    }

    auto& registry = m_timeline->getRegistry();

    if (!registry.valid(selectedClip)) {
        ImGui::TextDisabled("Invalid selection");
        return;
    }

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

    // Position
    ImGui::Text("Position");
    ImGui::SetNextItemWidth(-1);
    float pos[3] = { transform->position.x, transform->position.y, transform->position.z };
    if (ImGui::DragFloat3("##position", pos, 1.0f, -10000.0f, 10000.0f, "%.1f")) {
        transform->setPosition(glm::vec3(pos[0], pos[1], pos[2]));
    }

    // Rotation (already stored in degrees in Transform)
    ImGui::Text("Rotation");
    ImGui::SetNextItemWidth(-1);
    float rot[3] = { transform->rotation.x, transform->rotation.y, transform->rotation.z };
    if (ImGui::DragFloat3("##rotation", rot, 0.5f, -360.0f, 360.0f, "%.1f deg")) {
        transform->setRotation(glm::vec3(rot[0], rot[1], rot[2]));
    }

    // Scale
    ImGui::Text("Scale");
    ImGui::SetNextItemWidth(-1);
    float scale[3] = { transform->scale.x, transform->scale.y, transform->scale.z };
    if (ImGui::DragFloat3("##scale", scale, 0.01f, 0.01f, 100.0f, "%.3f")) {
        transform->setScale(glm::vec3(scale[0], scale[1], scale[2]));
    }

    // Uniform scale checkbox
    static bool uniformScale = true;
    ImGui::Checkbox("Uniform Scale", &uniformScale);

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

} // namespace entity
