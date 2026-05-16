#include "entity/ui/ScreensWindow.hpp"
#include "entity/core/Engine.hpp"
#include "entity/timeline/Timeline.hpp"
#include "entity/components/Screen.hpp"
#include "entity/components/Model.hpp"
#include "entity/components/Projector.hpp"
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
                    applyModelToScreen(screen, modelEntity, registry.try_get<Model>(modelEntity));
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

    // ---- Projectors section ----
    ImGui::Separator();

    ImGui::Text("Projectors");
    ImGui::SameLine();
    if (ImGui::SmallButton("+ Add##proj")) {
        entt::entity newProj = createProjector();
        if (timeline) {
            timeline->setSelectedProjector(newProj);
            timeline->setSelectedClip(entt::null);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Virtual cameras that map content onto\nProjection Surface screens.");
    }

    ImGui::Separator();

    entt::entity selectedProjector = timeline ? timeline->getSelectedProjector() : entt::null;
    auto projView = registry.view<Projector>();
    int projIndex = 0;

    for (auto projEntity : projView) {
        Projector& proj = projView.get<Projector>(projEntity);

        ImGui::PushID(static_cast<int>(projEntity) + 100000);

        bool isSelProj = (projEntity == selectedProjector);

        // Enabled checkbox inline
        ImGui::Checkbox("##en", &proj.enabled);
        ImGui::SameLine();

        if (ImGui::Selectable(proj.name.c_str(), isSelProj)) {
            if (timeline) {
                timeline->setSelectedProjector(projEntity);
                timeline->setSelectedClip(entt::null);
            }
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Delete")) {
                registry.destroy(projEntity);
                if (timeline && selectedProjector == projEntity)
                    timeline->setSelectedProjector(entt::null);
            }
            if (ImGui::MenuItem("Duplicate")) {
                entt::entity newEntity = registry.create();
                registry.emplace<Projector>(newEntity, proj);
                Projector& newProj = registry.get<Projector>(newEntity);
                newProj.name = proj.name + " Copy";
                if (timeline) {
                    timeline->setSelectedProjector(newEntity);
                    timeline->setSelectedClip(entt::null);
                }
            }
            ImGui::EndPopup();
        }

        ImGui::PopID();
        projIndex++;
    }

    if (projIndex == 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No projectors.");
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
        entt::entity firstModel = *modelView.begin();
        applyModelToScreen(screen, firstModel, registry.try_get<Model>(firstModel));
    }

    // Sit the screen on the floor by default — position.y is the center of
    // the quad, so size.y/2 puts the bottom edge at world Y=0.
    screen.position[1] = screen.size[1] * 0.5f;

    std::cout << "[Screens] Created screen: " << name << " (entity=" << static_cast<uint32_t>(entity) << ")" << std::endl;
    return entity;
}

entt::entity ScreensWindow::createProjector() {
    auto& registry = m_engine->getRegistry();
    entt::entity entity = registry.create();
    Projector& proj = registry.emplace<Projector>(entity);

    // Auto-number: "Projector 1", "Projector 2", ...
    int count = 1;
    for (auto e : registry.view<Projector>()) {
        (void)e;
        count++;
    }
    proj.name = "Projector " + std::to_string(count);

    std::cout << "[Screens] Created projector: " << proj.name << std::endl;
    return entity;
}

} // namespace entity
