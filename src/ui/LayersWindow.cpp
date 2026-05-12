#include "entity/ui/LayersWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Layer.hpp"
#include <imgui.h>
#include <iostream>

namespace entity {

LayersWindow::LayersWindow(Engine* engine)
    : m_engine(engine)
{
}

void LayersWindow::render() {
    if (!m_engine) return;

    ImGui::TextDisabled("Drag a layer kind onto a timeline track.");
    ImGui::Separator();
    ImGui::Spacing();

    // Clip layer drag source
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.31f, 0.47f, 0.70f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.41f, 0.57f, 0.80f, 1.0f));
        ImGui::Button("Clip", ImVec2(-1.0f, 40.0f));
        ImGui::PopStyleColor(2);

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Drag onto a track to create an empty clip.\n"
                              "Assign media via the Properties panel.");
        }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            uint8_t kind = static_cast<uint8_t>(Layer::Kind::Clip);
            ImGui::SetDragDropPayload("LAYER_KIND", &kind, sizeof(kind));
            ImGui::Text("Clip");
            ImGui::EndDragDropSource();
        }
    }

    ImGui::Spacing();

    // Object Animation layer drag source
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.35f, 0.70f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.45f, 0.80f, 1.0f));
        ImGui::Button("Object Animation", ImVec2(-1.0f, 40.0f));
        ImGui::PopStyleColor(2);

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Drag onto a track to create an Object Animation layer.\n"
                              "Pick a target Screen or Prop in the Properties panel,\n"
                              "then add keyframes to animate its position/rotation/scale.");
        }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            uint8_t kind = static_cast<uint8_t>(Layer::Kind::ObjectAnimation);
            ImGui::SetDragDropPayload("LAYER_KIND", &kind, sizeof(kind));
            ImGui::Text("Object Animation");
            ImGui::EndDragDropSource();
        }
    }

    ImGui::Spacing();

    // Generative layer drag source — first kind is "Muncher" (Pac-Man-style
    // maze chase with the lower-case-"e" Entity logo as the player). Future
    // generative kinds (particles, visualizers, ...) get their own buttons
    // here.
    {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.66f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.88f, 0.76f, 0.28f, 1.0f));
        ImGui::Button("Generative — Muncher", ImVec2(-1.0f, 40.0f));
        ImGui::PopStyleColor(2);

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Drag onto a track to create a Muncher generative layer.\n"
                              "A maze-chase mini-game rendered into the layer's render target;\n"
                              "responds to external inputs (OSC / MIDI / keyboard) over time.");
        }

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            uint8_t kind = static_cast<uint8_t>(Layer::Kind::Generative);
            ImGui::SetDragDropPayload("LAYER_KIND", &kind, sizeof(kind));
            ImGui::Text("Muncher");
            ImGui::EndDragDropSource();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("Layer kinds:");
    ImGui::BulletText("Clip — video or image source");
    ImGui::BulletText("Object Animation — keyframed\ntransform on a Screen or Prop");
    ImGui::BulletText("Generative — procedural\ntexture (Muncher, ...)");
}

} // namespace entity
