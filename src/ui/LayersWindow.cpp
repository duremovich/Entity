#include "entity/ui/LayersWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Layer.hpp"
#include <imgui.h>

namespace entity {

namespace {

// One row in the layer palette: tinted color swatch + label, draggable.
// Payload is two bytes: [Layer::Kind, subKind]. subKind is 0 for Clip / OA /
// Muncher (ignored by the accept side for non-Generative kinds), and 1 for
// Text. TimelineWidget reads both bytes to dispatch to the right callback.
void layerPaletteRow(const char* label,
                     const char* tooltip,
                     Layer::Kind kind,
                     ImVec4 swatch,
                     uint8_t subKind = 0)
{
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const float swatchSize = lineH * 0.55f;
    ImVec2 cursor = ImGui::GetCursorScreenPos();

    // Reserve a Selectable spanning the row width — gives us hover/click feedback
    // and acts as the drag source. We push a unique ID per entry to disambiguate
    // labels (future kinds may reuse a short name across categories).
    ImGui::PushID(label);

    // Indent the swatch+label by a few pixels so it doesn't kiss the category
    // header column.
    bool clicked = ImGui::Selectable("##row", false,
                                     ImGuiSelectableFlags_AllowItemOverlap,
                                     ImVec2(0.0f, lineH));
    (void)clicked;  // selection isn't meaningful here — drag is the action

    // Draw swatch + label on top of the selectable.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float swatchY = cursor.y + (lineH - swatchSize) * 0.5f;
    float swatchX = cursor.x + 6.0f;
    ImU32 col = ImGui::ColorConvertFloat4ToU32(swatch);
    dl->AddRectFilled(ImVec2(swatchX, swatchY),
                      ImVec2(swatchX + swatchSize, swatchY + swatchSize),
                      col, 2.0f);
    dl->AddRect(ImVec2(swatchX, swatchY),
                ImVec2(swatchX + swatchSize, swatchY + swatchSize),
                IM_COL32(0, 0, 0, 160), 2.0f);
    dl->AddText(ImVec2(swatchX + swatchSize + 8.0f, cursor.y + 1.0f),
                IM_COL32(220, 220, 220, 255), label);

    if (ImGui::IsItemHovered() && tooltip) {
        ImGui::SetTooltip("%s", tooltip);
    }

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        uint8_t payload[2] = {static_cast<uint8_t>(kind), subKind};
        ImGui::SetDragDropPayload("LAYER_KIND", payload, sizeof(payload));
        ImGui::Text("%s", label);
        ImGui::EndDragDropSource();
    }

    ImGui::PopID();
}

void categoryComingSoon(const char* note) {
    ImGui::TextDisabled("    %s", note);
}

} // namespace

LayersWindow::LayersWindow(Engine* engine)
    : m_engine(engine)
{
}

void LayersWindow::render() {
    if (!m_engine) return;

    ImGui::TextDisabled("Drag a layer onto a timeline track.");
    ImGui::Spacing();

    // === Video ===
    if (ImGui::CollapsingHeader("Video", ImGuiTreeNodeFlags_DefaultOpen)) {
        layerPaletteRow(
            "Clip",
            "Drag onto a track to create an empty clip.\n"
            "Assign media via the Properties panel.",
            Layer::Kind::Clip,
            ImVec4(0.31f, 0.47f, 0.70f, 1.0f));

        layerPaletteRow(
            "Muncher",
            "Drag onto a track to create a Muncher generative layer.\n"
            "A maze-chase mini-game rendered into the layer's render target;\n"
            "responds to external inputs (OSC / MIDI / keyboard) over time.",
            Layer::Kind::Generative,
            ImVec4(0.85f, 0.78f, 0.20f, 1.0f),
            0);  // subKind 0 = Muncher

        layerPaletteRow(
            "Text",
            "Drag onto a track to create a Text generative layer.\n"
            "Renders a text string into the layer's render target.\n"
            "Set content, font, and color in the Properties panel.",
            Layer::Kind::Generative,
            ImVec4(0.20f, 0.72f, 0.85f, 1.0f),
            1);  // subKind 1 = Text

        layerPaletteRow(
            "Solid",
            "Drag onto a track to create a Solid color layer.\n"
            "A flat color fill — the classic host for generator effects\n"
            "(noise, gradients) and effect-chain experiments.\n"
            "Set color and resolution in the Properties panel.",
            Layer::Kind::Generative,
            ImVec4(0.62f, 0.62f, 0.62f, 1.0f),
            2);  // subKind 2 = Solid
    }

    // === Previz ===
    // Layer kinds that drive the pre-vis stage (Screen / Prop transforms,
    // camera moves, etc.) rather than producing on-output texture content.
    if (ImGui::CollapsingHeader("Previz", ImGuiTreeNodeFlags_DefaultOpen)) {
        layerPaletteRow(
            "Object Animation",
            "Drag onto a track to create an Object Animation layer.\n"
            "Pick a target Screen or Prop in the Properties panel,\n"
            "then add keyframes to animate its position/rotation/scale.",
            Layer::Kind::ObjectAnimation,
            ImVec4(0.55f, 0.35f, 0.70f, 1.0f));
    }

    // === Audio ===
    if (ImGui::CollapsingHeader("Audio")) {
        categoryComingSoon("(no audio layer kinds yet)");
    }

    // === Control ===
    if (ImGui::CollapsingHeader("Control", ImGuiTreeNodeFlags_DefaultOpen)) {
        layerPaletteRow(
            "Signal Output",
            "Drag onto a track to create a Signal Output layer.\n"
            "Emits OSC packets at a keyframed or progress-mapped value\n"
            "while the playhead is inside the layer window.\n"
            "Set address, mode, and args in the Properties panel.",
            Layer::Kind::Signal,
            ImVec4(0.90f, 0.60f, 0.20f, 1.0f));
    }
}

} // namespace entity
