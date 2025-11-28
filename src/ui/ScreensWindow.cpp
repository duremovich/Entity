#include "entity/ui/ScreensWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Model.hpp"
#include <iostream>

namespace entity {

ScreensWindow::ScreensWindow(Engine* engine)
    : m_engine(engine)
{
}

void ScreensWindow::render() {
    if (!m_engine) return;

    auto& registry = m_engine->getRegistry();
    Timeline* timeline = m_engine->getTimeline();

    // Toolbar
    if (ImGui::Button("+ Add Screen")) {
        entt::entity newScreen = createScreen();
        if (timeline) {
            timeline->setSelectedScreen(newScreen);
            timeline->setSelectedClip(entt::null);  // Deselect clip when selecting screen
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Click a screen to select it.\nProperties appear in the Properties window.");
    }

    ImGui::Separator();

    // Get selected screen from timeline
    entt::entity selectedScreen = timeline ? timeline->getSelectedScreen() : entt::null;

    // List all screens
    auto view = registry.view<Screen>();
    int screenIndex = 0;

    for (auto entity : view) {
        Screen& screen = view.get<Screen>(entity);

        ImGui::PushID(static_cast<int>(entity));

        bool isSelected = (entity == selectedScreen);

        // Screen item - show name and resolution
        char label[256];
        snprintf(label, sizeof(label), "%s (%dx%d)", screen.name.c_str(), screen.width, screen.height);

        if (ImGui::Selectable(label, isSelected)) {
            if (timeline) {
                timeline->setSelectedScreen(entity);
                timeline->setSelectedClip(entt::null);  // Deselect clip when selecting screen
            }
        }

        // Accept model drops to assign geometry
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("MODEL_ENTITY")) {
                uint32_t modelEntityId = *static_cast<const uint32_t*>(payload->Data);
                entt::entity modelEntity = static_cast<entt::entity>(modelEntityId);

                if (registry.valid(modelEntity) && registry.all_of<Model>(modelEntity)) {
                    screen.modelEntity = modelEntity;
                    std::cout << "[Screens] Assigned model to " << screen.name << std::endl;
                }
            }
            ImGui::EndDragDropTarget();
        }

        // Right-click context menu
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Delete")) {
                registry.destroy(entity);
                if (timeline && selectedScreen == entity) {
                    timeline->setSelectedScreen(entt::null);
                }
            }
            if (ImGui::MenuItem("Duplicate")) {
                entt::entity newEntity = registry.create();
                registry.emplace<Screen>(newEntity, screen);
                Screen& newScreen = registry.get<Screen>(newEntity);
                newScreen.name = screen.name + " Copy";
                if (timeline) {
                    timeline->setSelectedScreen(newEntity);
                    timeline->setSelectedClip(entt::null);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
        screenIndex++;
    }

    if (screenIndex == 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No screens.");
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Click '+ Add Screen'");
    }
}

entt::entity ScreensWindow::createScreen(const char* name) {
    auto& registry = m_engine->getRegistry();

    entt::entity entity = registry.create();
    Screen& screen = registry.emplace<Screen>(entity);
    screen.name = name;

    // Use first available model if any exist
    auto modelView = registry.view<Model>();
    if (modelView.size() > 0) {
        screen.modelEntity = *modelView.begin();
    }

    std::cout << "[Screens] Created screen: " << name << " (entity=" << static_cast<uint32_t>(entity) << ")" << std::endl;
    return entity;
}

} // namespace entity
