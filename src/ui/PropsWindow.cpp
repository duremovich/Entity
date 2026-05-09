#include "entity/ui/PropsWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Prop.hpp"
#include "entity/components/Model.hpp"
#include <iostream>

namespace entity {

PropsWindow::PropsWindow(Engine* engine)
    : m_engine(engine)
{
}

void PropsWindow::render() {
    if (!m_engine) return;

    auto& registry = m_engine->getRegistry();
    Timeline* timeline = m_engine->getTimeline();

    if (ImGui::Button("+ Add Prop")) {
        entt::entity newProp = createProp();
        if (timeline) {
            timeline->setSelectedProp(newProp);
            timeline->setSelectedClip(entt::null);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Props are 3D set pieces visible in the stage view\n"
                          "but never sent to projector output.\n"
                          "Drag a model onto a prop to assign geometry.");
    }

    ImGui::Separator();

    entt::entity selectedProp = timeline ? timeline->getSelectedProp() : entt::null;

    auto view = registry.view<Prop>();
    int propIndex = 0;

    for (auto entity : view) {
        Prop& prop = view.get<Prop>(entity);

        ImGui::PushID(static_cast<int>(entity));

        bool isSelected = (entity == selectedProp);

        // Visible checkbox inline so designers can quickly toggle props off
        // without losing selection or property panel state.
        ImGui::Checkbox("##vis", &prop.visible);
        ImGui::SameLine();

        if (ImGui::Selectable(prop.name.c_str(), isSelected)) {
            if (timeline) {
                timeline->setSelectedProp(entity);
                timeline->setSelectedClip(entt::null);
            }
        }

        // Drag-drop a model to assign geometry — same payload type Screens use.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODEL_ENTITY")) {
                uint32_t modelEntityId = *static_cast<const uint32_t*>(payload->Data);
                entt::entity modelEntity = static_cast<entt::entity>(modelEntityId);

                if (registry.valid(modelEntity) && registry.all_of<Model>(modelEntity)) {
                    prop.modelEntity = modelEntity;
                    std::cout << "[Props] Assigned model to " << prop.name << std::endl;
                }
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Delete")) {
                registry.destroy(entity);
                if (timeline && selectedProp == entity) {
                    timeline->setSelectedProp(entt::null);
                }
            }
            if (ImGui::MenuItem("Duplicate")) {
                entt::entity newEntity = registry.create();
                registry.emplace<Prop>(newEntity, prop);
                Prop& newProp = registry.get<Prop>(newEntity);
                newProp.name = prop.name + " Copy";
                if (timeline) {
                    timeline->setSelectedProp(newEntity);
                    timeline->setSelectedClip(entt::null);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
        propIndex++;
    }

    if (propIndex == 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No props.");
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Click '+ Add Prop'");
    }
}

entt::entity PropsWindow::createProp(const char* name) {
    auto& registry = m_engine->getRegistry();

    entt::entity entity = registry.create();
    Prop& prop = registry.emplace<Prop>(entity);

    // Auto-number to keep the list legible: "Prop", "Prop 2", "Prop 3", …
    int count = 0;
    for (auto e : registry.view<Prop>()) {
        (void)e;
        count++;
    }
    prop.name = (count <= 1) ? std::string(name)
                             : std::string(name) + " " + std::to_string(count);

    // Auto-assign first available Model (mirrors ScreensWindow behavior).
    auto modelView = registry.view<Model>();
    if (modelView.size() > 0) {
        prop.modelEntity = *modelView.begin();
    }

    std::cout << "[Props] Created prop: " << prop.name
              << " (entity=" << static_cast<uint32_t>(entity) << ")" << std::endl;
    return entity;
}

} // namespace entity
