#include "entity/ui/ModelBinWindow.hpp"
#include "entity/ui/WindowManager.hpp"
#include "entity/core/Engine.hpp"
#include "entity/components/Model.hpp"
#include "entity/media/ObjLoader.hpp"
#include <iostream>

namespace entity {

ModelBinWindow::ModelBinWindow(Engine* engine, WindowManager* windowManager)
    : m_engine(engine)
    , m_windowManager(windowManager)
{
}

void ModelBinWindow::render() {
    if (!m_engine) return;

    auto& registry = m_engine->getRegistry();

    // Toolbar
    if (ImGui::Button("Import OBJ")) {
        if (m_windowManager) {
            std::string filePath = m_windowManager->openOBJFileDialog();
            if (!filePath.empty()) {
                importModel(filePath);
            }
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Create Default")) {
        // Create a default 16:9 screen plane
        entt::entity modelEntity = registry.create();
        Model& model = registry.emplace<Model>(modelEntity);
        model.name = "Default 16:9 Plane";
        model.filepath = "";
        model.mesh = createDefaultScreenMesh();
        std::cout << "[ModelBin] Created default 16:9 plane" << std::endl;
    }

    ImGui::Separator();

    // Model list
    auto view = registry.view<Model>();
    int modelCount = 0;

    ImGui::Text("Loaded Models: %d", static_cast<int>(view.size()));
    ImGui::Separator();

    // Table of models
    if (ImGui::BeginTable("ModelsTable", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Vertices", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();

        for (auto entity : view) {
            const Model& model = view.get<Model>(entity);

            ImGui::TableNextRow();

            // Name
            ImGui::TableNextColumn();
            bool selected = false;
            ImGui::Selectable(model.name.c_str(), &selected, ImGuiSelectableFlags_SpanAllColumns);

            // Make model draggable for assigning to screens
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                uint32_t entityId = static_cast<uint32_t>(entity);
                ImGui::SetDragDropPayload("MODEL_ENTITY", &entityId, sizeof(entityId));
                ImGui::Text("Model: %s", model.name.c_str());
                ImGui::EndDragDropSource();
            }

            // Tooltip with details
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Name: %s", model.name.c_str());
                if (!model.filepath.empty()) {
                    ImGui::Text("File: %s", model.filepath.c_str());
                }
                ImGui::Text("Vertices: %zu", model.mesh.vertices.size());
                ImGui::Text("Triangles: %zu", model.mesh.indices.size() / 3);

                // Bounds
                const auto& min = model.mesh.minBounds;
                const auto& max = model.mesh.maxBounds;
                ImGui::Text("Bounds: (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)",
                           min[0], min[1], min[2], max[0], max[1], max[2]);
                ImGui::EndTooltip();
            }

            // Vertices
            ImGui::TableNextColumn();
            ImGui::Text("%zu", model.mesh.vertices.size());

            // Triangles
            ImGui::TableNextColumn();
            ImGui::Text("%zu", model.mesh.indices.size() / 3);

            // Actions
            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(entity));
            if (ImGui::SmallButton("X")) {
                // Delete model (will be processed after loop)
                registry.destroy(entity);
            }
            ImGui::PopID();

            modelCount++;
        }

        ImGui::EndTable();
    }

    if (modelCount == 0) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No models imported.");
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Click 'Import OBJ' or 'Create Default' to add models.");
    }

    // Handle OBJ file drops
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("EXTERNAL_FILE")) {
            const char* filepath = static_cast<const char*>(payload->Data);
            std::string path(filepath, payload->DataSize);

            // Check if it's an OBJ file
            if (path.size() > 4) {
                std::string ext = path.substr(path.size() - 4);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".obj") {
                    importModel(path);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
}

void ModelBinWindow::importModel(const std::string& filepath) {
    auto& registry = m_engine->getRegistry();

    MeshData mesh = ObjLoader::load(filepath);
    if (!mesh.isValid()) {
        std::cerr << "[ModelBin] Failed to load OBJ: " << filepath << std::endl;
        return;
    }

    entt::entity modelEntity = registry.create();
    Model& model = registry.emplace<Model>(modelEntity);
    model.name = mesh.name;
    model.filepath = filepath;
    model.mesh = std::move(mesh);

    std::cout << "[ModelBin] Imported model: " << model.name << std::endl;
}

} // namespace entity
