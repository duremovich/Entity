#include "entity/ui/ModelBinWindow.hpp"
#include "entity/ui/WindowManager.hpp"
#include "entity/core/Engine.hpp"
#include "entity/components/Model.hpp"
#include "entity/project/MediaVersioning.hpp"
#include "entity/project/ProjectManager.hpp"
#include <algorithm>
#include <filesystem>
#include <iostream>

namespace entity {

ModelBinWindow::ModelBinWindow(Engine* engine, WindowManager* windowManager)
    : m_engine(engine)
    , m_windowManager(windowManager)
{
}

void ModelBinWindow::renderPendingImportModal() {
    const Engine::PendingObjectImport* pending = m_engine->pendingObjectImport();

    if (pending) {
        if (pending->sourceFilePath != m_modalLastFilepath) {
            m_modalLastFilepath = pending->sourceFilePath;
            m_modalChosenMode      = pending->storageDecided
                ? static_cast<int>(pending->resolvedMode)
                : 1;  // Copy default — matches the media modal's muscle memory
            m_modalSubfolderBuf[0] = '\0';
            m_modalDontAskStorage  = false;
            ImGui::OpenPopup("Import Object");
        }
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0));

    if (ImGui::BeginPopupModal("Import Object", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string& path =
            pending ? pending->sourceFilePath : m_modalLastFilepath;
        const size_t lastSlash = path.find_last_of("/\\");
        const std::string stem = (lastSlash != std::string::npos)
            ? path.substr(lastSlash + 1) : path;

        ImGui::TextDisabled("Importing:");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(stem.c_str());
        ImGui::PopTextWrapPos();
        ImGui::TextDisabled("%s", path.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool askStorage = pending && !pending->storageDecided;

        if (askStorage) {
            ImGui::TextUnformatted("Storage");
            ImGui::RadioButton("Copy into project##obj_store", &m_modalChosenMode, 1);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Copy the file into <project>/objects/. Travels with the\n"
                    "project; portable across machines.");
            }
            ImGui::RadioButton("Link to original location##obj_store", &m_modalChosenMode, 0);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Reference the file at its current absolute path.\n"
                    "Not portable without relinking.");
            }

            ImGui::BeginDisabled(m_modalChosenMode != 1);
            ImGui::TextDisabled("objects/");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220.0f);
            ImGui::InputTextWithHint("##obj_subfolder", "(optional subfolder)",
                                      m_modalSubfolderBuf,
                                      sizeof(m_modalSubfolderBuf));
            if (ImGui::IsItemHovered() && m_modalChosenMode == 1) {
                ImGui::SetTooltip(
                    "Optional subfolder under objects/ for this import.\n"
                    "Created on demand. Leave blank to land in objects/ root.");
            }
            ImGui::EndDisabled();

            ImGui::Checkbox("Don't ask Storage again##obj_store", &m_modalDontAskStorage);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Persist this Storage choice (Copy or Link) as the default\n"
                    "for all future imports (media and 3D models share the\n"
                    "preference).");
            }
            ImGui::Spacing();
        }

        ImGui::Separator();
        ImGui::Spacing();

        const float btnWidth = 120.0f;
        if (ImGui::Button("Import", ImVec2(btnWidth, 0))) {
            const auto mode = static_cast<Engine::ImportMode>(m_modalChosenMode);
            const std::string subfolder = m_modalSubfolderBuf;
            m_engine->resolvePendingObjectImport(
                mode, subfolder,
                /*rememberStorage*/ askStorage && m_modalDontAskStorage);
            m_modalLastFilepath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(btnWidth, 0))) {
            // Cancel = Link without remember (mirror of media's cancel).
            m_engine->resolvePendingObjectImport(
                Engine::ImportMode::Link, /*subfolder*/ "",
                /*rememberStorage*/ false);
            m_modalLastFilepath.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    } else if (pending && m_modalLastFilepath == pending->sourceFilePath) {
        m_engine->resolvePendingObjectImport(
            Engine::ImportMode::Link, "", false);
        m_modalLastFilepath.clear();
    }
}

void ModelBinWindow::renderPendingDuplicateModal() {
    const Engine::PendingObjectDuplicate* dup = m_engine->pendingObjectDuplicate();

    if (dup) {
        if (dup->sourceFilePath != m_dupModalLastSource) {
            m_dupModalLastSource = dup->sourceFilePath;
            ImGui::OpenPopup("Object Already Exists");
        }
    }

    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(540, 0));

    if (ImGui::BeginPopupModal("Object Already Exists", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string subfolderDisplay =
            (dup ? dup->subfolder : std::string{});
        const std::string targetDir = subfolderDisplay.empty()
            ? std::string("objects/")
            : ("objects/" + subfolderDisplay + "/");

        ImGui::TextUnformatted("A file with this name already exists in the");
        ImGui::Text("project at %s", targetDir.c_str());
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.5f, 1.0f), "%s",
                           dup ? dup->targetFilename.c_str() : "");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted("What would you like to do?");
        ImGui::Spacing();

        const float btnWidth = 150.0f;

        if (ImGui::Button("Replace", ImVec2(btnWidth, 0))) {
            m_engine->resolvePendingObjectDuplicate(
                Engine::DuplicateResolution::Replace);
            m_dupModalLastSource.clear();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Overwrite the existing file. The previous mesh is lost.\n"
                "Any Model using this name picks up the new geometry on\n"
                "the next frame.");
        }

        ImGui::SameLine();
        const std::string keepBothLabel = std::string("Keep both (") +
            (dup ? dup->suggestedRenamed : std::string("...")) + ")";
        if (ImGui::Button(keepBothLabel.c_str(), ImVec2(0, 0))) {
            m_engine->resolvePendingObjectDuplicate(
                Engine::DuplicateResolution::KeepBoth);
            m_dupModalLastSource.clear();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Auto-suffix the new file's name so both versions live\n"
                "side-by-side under objects/. The existing file is untouched.");
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(btnWidth, 0))) {
            m_engine->cancelPendingObjectDuplicate();
            m_dupModalLastSource.clear();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "Abandon the import. Nothing is written to disk.");
        }

        ImGui::EndPopup();
    } else if (dup && m_dupModalLastSource == dup->sourceFilePath) {
        // Popup dismissed externally (ESC etc.) — treat as Cancel so the
        // pending state doesn't strand on Engine.
        m_engine->cancelPendingObjectDuplicate();
        m_dupModalLastSource.clear();
    }
}

void ModelBinWindow::render() {
    if (!m_engine) return;

    auto& registry = m_engine->getRegistry();
    auto* projectMgr = m_engine->getProjectManager();

    // Toolbar
    const bool importBusy = m_windowManager && m_windowManager->isFileDialogPending();
    ImGui::BeginDisabled(importBusy);
    if (ImGui::Button(importBusy ? "Importing..." : "Import OBJ")) {
        if (m_windowManager) {
            m_windowManager->openOBJFileDialog([this](const std::string& filePath) {
                if (!filePath.empty()) m_engine->onModelFileSelected(filePath);
            });
        }
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    auto view = registry.view<Model>();

    ImGui::Text("Loaded Models: %d", static_cast<int>(view.size()));
    ImGui::Separator();

    entt::entity toDelete = entt::null;

    if (ImGui::BeginTable("ModelsTable", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Vertices", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();

        for (auto entity : view) {
            const Model& model = view.get<Model>(entity);

            // Collect version members for this Model's logical group from
            // the project's object library, if any. One Model entity per
            // group (ingestObjectFile guarantees that), so this is
            // tooltip-only enrichment.
            std::vector<const ProjectManager::ObjectLibraryEntry*> versionMembers;
            std::string latestTag;
            if (projectMgr && !model.filepath.empty()) {
                const std::string groupKey = groupKeyOf(model.filepath);
                for (const auto& e : projectMgr->loadedObjectFiles()) {
                    if (groupKeyOf(e.originalPath) == groupKey) {
                        versionMembers.push_back(&e);
                    }
                }
                std::stable_sort(versionMembers.begin(), versionMembers.end(),
                    [](const auto* a, const auto* b) {
                        const std::string aTag = parseVersion(
                            std::filesystem::path(a->originalPath).stem().string()).tag;
                        const std::string bTag = parseVersion(
                            std::filesystem::path(b->originalPath).stem().string()).tag;
                        return compareVersionTags(aTag, bTag) < 0;
                    });
                if (!versionMembers.empty()) {
                    latestTag = parseVersion(
                        std::filesystem::path(versionMembers.back()->originalPath).stem().string()).tag;
                }
            }

            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            const std::string displayName = latestTag.empty()
                ? model.name
                : (model.name + "  [v" + latestTag + "]");
            bool selected = false;
            ImGui::Selectable(displayName.c_str(), &selected,
                              ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap);

            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
                uint32_t entityId = static_cast<uint32_t>(entity);
                ImGui::SetDragDropPayload("MODEL_ENTITY", &entityId, sizeof(entityId));
                ImGui::Text("Model: %s", model.name.c_str());
                ImGui::EndDragDropSource();
            }

            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Name: %s", model.name.c_str());
                if (!model.filepath.empty()) {
                    ImGui::Text("Logical: %s", model.filepath.c_str());
                }
                ImGui::Text("Vertices: %zu", model.mesh.vertices.size());
                ImGui::Text("Triangles: %zu", model.mesh.indices.size() / 3);
                const auto& mn = model.mesh.minBounds;
                const auto& mx = model.mesh.maxBounds;
                ImGui::Text("Bounds: (%.2f, %.2f, %.2f) to (%.2f, %.2f, %.2f)",
                           mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
                if (versionMembers.size() > 1) {
                    ImGui::Separator();
                    ImGui::Text("Versions (%zu):", versionMembers.size());
                    for (const auto* m : versionMembers) {
                        const std::string tag = parseVersion(
                            std::filesystem::path(m->originalPath).stem().string()).tag;
                        const char* marker = (m == versionMembers.back()) ? " (latest)" : "";
                        if (tag.empty()) {
                            ImGui::Text("  %s%s", m->originalPath.c_str(), marker);
                        } else {
                            ImGui::Text("  [v%s] %s%s", tag.c_str(),
                                        m->originalPath.c_str(), marker);
                        }
                    }
                }
                ImGui::EndTooltip();
            }

            ImGui::TableNextColumn();
            ImGui::Text("%zu", model.mesh.vertices.size());

            ImGui::TableNextColumn();
            ImGui::Text("%zu", model.mesh.indices.size() / 3);

            ImGui::TableNextColumn();
            ImGui::PushID(static_cast<int>(entity));
            if (ImGui::SmallButton("X"))
                toDelete = entity;
            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    if (toDelete != entt::null) {
        // Also drop the object library entries for this group so
        // ContentScanner doesn't re-ingest the same files on the next
        // tick. The file on disk is left alone — destructive removal
        // is a separate UX.
        if (projectMgr) {
            const auto& doomed = registry.get<Model>(toDelete);
            const std::string groupKey = groupKeyOf(doomed.filepath);
            std::vector<std::string> toRemove;
            for (const auto& e : projectMgr->loadedObjectFiles()) {
                if (groupKeyOf(e.originalPath) == groupKey) {
                    toRemove.push_back(e.originalPath);
                }
            }
            for (const auto& p : toRemove) projectMgr->removeObjectFile(p);
        }
        registry.destroy(toDelete);
    }

    if (view.size() == 0u) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "No models imported.");
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "Click 'Import OBJ' or drop a .obj file here.");
    }

    // External file drop — only OBJ for v1. glTF/FBX expansion is a
    // future card; the scanner whitelist matches.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("EXTERNAL_FILE")) {
            const char* filepath = static_cast<const char*>(payload->Data);
            std::string path(filepath, payload->DataSize);
            if (path.size() > 4) {
                std::string ext = path.substr(path.size() - 4);
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".obj") {
                    m_engine->onModelFileSelected(path);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    renderPendingImportModal();
    renderPendingDuplicateModal();
}

} // namespace entity
