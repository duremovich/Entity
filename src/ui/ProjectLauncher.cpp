/**
 * ProjectLauncher implementation. See header for the rationale.
 */

#include "entity/ui/ProjectLauncher.hpp"

#include "entity/project/ProjectManager.hpp"
#include "entity/project/RecentProjects.hpp"
#include "entity/ui/FileDialog.hpp"

#include <imgui.h>

#include <GLFW/glfw3.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>

namespace fs = std::filesystem;

namespace entity {

namespace {

std::string formatTimestamp(std::int64_t unixSeconds) {
    if (unixSeconds <= 0) return "—";
    std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

}  // namespace

void ProjectLauncher::initialize(ProjectManager* pm, RecentProjects* rp, void* glfwWindow) {
    m_projectManager = pm;
    m_recentProjects = rp;
    m_glfwWindow     = glfwWindow;
}

void ProjectLauncher::reset() {
    m_pending = {};
    m_newProjectOpen = false;
    m_newProjectName.clear();
    m_newProjectParentDir.clear();
    m_newProjectError.clear();
    // Note: do NOT reset m_pendingDialog — its destructor would have to
    // join/detach a worker that might still be inside IFileOpenDialog::Show().
    // Letting any in-flight dialog finish naturally and consume on next
    // pumpPendingDialog() is cheaper and safer.
}

void ProjectLauncher::pumpPendingDialog() {
    if (!m_pendingDialog || !m_pendingDialog->isDone()) return;

    fs::path picked = m_pendingDialog->result();
    m_pendingDialog.reset();
    auto cb = std::move(m_pendingDialogCallback);
    m_pendingDialogCallback = nullptr;
    if (cb) cb(std::move(picked));

    if (m_glfwWindow) {
        glfwFocusWindow(static_cast<GLFWwindow*>(m_glfwWindow));
    }
}

void ProjectLauncher::startDialog(std::unique_ptr<ui::FileDialogTask> task,
                                   std::function<void(fs::path)> onComplete) {
    m_pendingDialog         = std::move(task);
    m_pendingDialogCallback = std::move(onComplete);
}

ProjectLauncher::Result ProjectLauncher::render() {
    // Cover the full main viewport with a borderless background — the
    // launcher is a full-window state, not a floating modal.
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags kBgFlags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize   | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40.0f, 32.0f));
    ImGui::Begin("##ProjectLauncherBg", nullptr, kBgFlags);
    ImGui::PopStyleVar();

    // Drain any in-flight dialog before drawing buttons so completed
    // results are visible to this frame's UI.
    pumpPendingDialog();

    ImGui::PushFont(nullptr);  // Use default font; could swap to a heading font later.
    ImGui::Text("Entity Media Server");
    ImGui::PopFont();
    ImGui::TextDisabled("Pick a project to get started.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Top action row.
    if (ImGui::Button("New Project...", ImVec2(160, 0))) {
        m_newProjectOpen = true;
        m_newProjectError.clear();
        // Pre-populate parent dir with the user's home / Documents on first open.
        if (m_newProjectParentDir.empty()) {
#ifdef _WIN32
            if (const char* userprofile = std::getenv("USERPROFILE")) {
                fs::path docs = fs::path(userprofile) / "Documents" / "Entity";
                m_newProjectParentDir = docs.string();
            }
#else
            if (const char* home = std::getenv("HOME")) {
                m_newProjectParentDir = (fs::path(home) / "Entity").string();
            }
#endif
        }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_pendingDialog != nullptr);
    if (ImGui::Button(m_pendingDialog ? "Opening..." : "Open Project...",
                      ImVec2(160, 0))) {
        std::vector<ui::FileDialogFilter> filters = {
            {L"Entity Project (*.entity)", L"*.entity"},
            {L"All Files (*.*)",            L"*.*"},
        };
        startDialog(
            ui::openFileDialogAsync(L"Open Entity Project", filters),
            [this](fs::path picked) {
                if (!picked.empty()) {
                    m_pending.action = Action::Open;
                    m_pending.path   = std::move(picked);
                }
            });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Quit", ImVec2(80, 0))) {
        m_pending.action = Action::Quit;
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Recent Projects");
    ImGui::Spacing();
    renderRecentList();

    ImGui::End();

    // "New Project" sub-modal floats on top of the background window.
    if (m_newProjectOpen) {
        renderNewProjectModal();
    }

    Result out = m_pending;
    if (m_pending.action != Action::None) {
        // Reset before returning so a re-shown launcher (e.g. after closing
        // a project) starts clean.
        m_pending = {};
    }
    return out;
}

void ProjectLauncher::renderRecentList() {
    if (!m_recentProjects || m_recentProjects->empty()) {
        ImGui::TextDisabled("(no recent projects)");
        return;
    }

    if (ImGui::BeginTable("##RecentTable", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY,
            ImVec2(0.0f, 320.0f))) {
        ImGui::TableSetupColumn("Project",      ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Last Opened",  ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("",             ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        const auto& entries = m_recentProjects->entries();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            ImGui::TableNextRow();
            ImGui::PushID(static_cast<int>(i));

            // Detect missing project so the user gets a hint instead of a
            // confusing "load failed" log line.
            std::error_code ec;
            const bool missing = !fs::exists(fs::path(e.path), ec);

            ImGui::TableSetColumnIndex(0);
            if (missing) {
                ImGui::TextDisabled("(missing) %s", e.path.c_str());
            } else {
                ImGui::TextUnformatted(e.path.c_str());
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(formatTimestamp(e.lastOpenedUnixSeconds).c_str());

            ImGui::TableSetColumnIndex(2);
            ImGui::BeginDisabled(missing);
            if (ImGui::SmallButton("Open")) {
                m_pending.action = Action::Open;
                m_pending.path   = e.path;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Forget")) {
                m_recentProjects->remove(e.path);
                m_recentProjects->save();
                ImGui::PopID();
                break;  // entries vector mutated; bail rendering this frame
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void ProjectLauncher::renderNewProjectModal() {
    ImGui::OpenPopup("New Entity Project");

    // Center the popup over the viewport.
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImVec2 center = ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                           viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 0));

    if (ImGui::BeginPopupModal("New Entity Project", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Project Name");
        char nameBuf[256];
        std::snprintf(nameBuf, sizeof(nameBuf), "%s", m_newProjectName.c_str());
        if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) {
            m_newProjectName = nameBuf;
        }
        ImGui::TextDisabled("Letters, numbers, dashes — no path separators.");
        ImGui::Spacing();

        ImGui::Text("Parent Directory");
        char parentBuf[1024];
        std::snprintf(parentBuf, sizeof(parentBuf), "%s", m_newProjectParentDir.c_str());
        if (ImGui::InputText("##parent", parentBuf, sizeof(parentBuf))) {
            m_newProjectParentDir = parentBuf;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(m_pendingDialog != nullptr);
        if (ImGui::Button(m_pendingDialog ? "Browsing..." : "Browse...")) {
            fs::path initial = m_newProjectParentDir.empty()
                                   ? fs::path{}
                                   : fs::path(m_newProjectParentDir);
            startDialog(
                ui::pickFolderDialogAsync(
                    L"Choose parent directory for the new project", initial),
                [this](fs::path picked) {
                    if (!picked.empty()) {
                        m_newProjectParentDir = picked.string();
                    }
                });
        }
        ImGui::EndDisabled();

        if (!m_newProjectName.empty() && !m_newProjectParentDir.empty()) {
            fs::path preview = fs::path(m_newProjectParentDir) / m_newProjectName;
            ImGui::TextDisabled("Will create: %s", preview.string().c_str());
        }

        if (!m_newProjectError.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                               m_newProjectError.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        const bool canCreate = !m_newProjectName.empty() &&
                               !m_newProjectParentDir.empty() &&
                               m_projectManager != nullptr;
        ImGui::BeginDisabled(!canCreate);
        if (ImGui::Button("Create", ImVec2(120, 0))) {
            std::string err;
            if (tryCreateProject(&err)) {
                m_newProjectOpen = false;
                ImGui::CloseCurrentPopup();
                m_pending.action = Action::Open;
                m_pending.path   = m_projectManager->projectPath();
            } else {
                m_newProjectError = err;
            }
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_newProjectOpen = false;
            m_newProjectError.clear();
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

bool ProjectLauncher::tryCreateProject(std::string* errorOut) {
    if (!m_projectManager) {
        if (errorOut) *errorOut = "Project subsystem not initialized.";
        return false;
    }

    fs::path parent(m_newProjectParentDir);
    std::error_code ec;
    if (!fs::exists(parent, ec)) {
        // Try to create the parent directory if it doesn't exist — a
        // common case the first time the user picks "Documents/Entity".
        fs::create_directories(parent, ec);
        if (ec) {
            if (errorOut) {
                *errorOut = "Cannot create parent directory: " + ec.message();
            }
            return false;
        }
    } else if (!fs::is_directory(parent, ec)) {
        if (errorOut) *errorOut = "Parent path is not a directory.";
        return false;
    }

    if (!m_projectManager->createNew(parent, m_newProjectName)) {
        if (errorOut) {
            *errorOut = "Failed to create project (see console for details).";
        }
        return false;
    }
    return true;
}

}  // namespace entity
