#include "entity/ui/WindowManager.hpp"
#include <imgui_internal.h>  // For DockBuilder API
#include <iostream>

#ifdef _WIN32
#include <Windows.h>
#include <commdlg.h>  // For GetOpenFileName
#endif

namespace entity {

WindowManager::WindowManager() {
    // Constructor
}

WindowManager::~WindowManager() {
    shutdown();
}

void WindowManager::initialize() {
    std::cout << "Initializing WindowManager..." << std::endl;
    m_firstFrame = true;
    m_layoutResetRequested = false;
}

void WindowManager::shutdown() {
    std::cout << "Shutting down WindowManager..." << std::endl;
    m_windows.clear();
}

void WindowManager::render() {
    // Create fullscreen dockspace over the main viewport
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    // Dockspace window flags
    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
    windowFlags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    windowFlags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    // Important: Set padding to 0 so dockspace fills entire window
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    // Begin the dockspace window
    ImGui::Begin("DockSpace Window", nullptr, windowFlags);
    ImGui::PopStyleVar();

    // Render menu bar
    renderMenuBar();

    // Create the dockspace
    ImGuiID dockspaceID = ImGui::GetID("MainDockSpace");
    m_dockspaceId = dockspaceID;

    // Create default layout on first frame or after reset
    if (m_firstFrame || m_layoutResetRequested) {
        createDefaultLayout(dockspaceID);
        m_firstFrame = false;
        m_layoutResetRequested = false;
    }

    // Create the dockspace node
    ImGui::DockSpace(dockspaceID, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    ImGui::End();

    // Render all registered windows
    for (auto& window : m_windows) {
        if (window->isVisible()) {
            // Apply pre-begin styles (e.g., window padding)
            window->applyPreBeginStyles();

            // Begin window with custom flags
            ImGuiWindowFlags flags = window->getWindowFlags();
            if (ImGui::Begin(window->getName(), nullptr, flags)) {
                window->render();
            }
            ImGui::End();

            // Pop pre-begin styles
            window->popPreBeginStyles();
        }
    }
}

void WindowManager::registerWindow(std::unique_ptr<EditorWindow> window) {
    std::cout << "Registering window: " << window->getName() << std::endl;
    m_windows.push_back(std::move(window));
}

void WindowManager::setWindowVisible(const char* name, bool visible) {
    for (auto& window : m_windows) {
        if (strcmp(window->getName(), name) == 0) {
            window->setVisible(visible);
            return;
        }
    }
}

void WindowManager::resetLayout() {
    std::cout << "Layout reset requested" << std::endl;
    m_layoutResetRequested = true;
}

void WindowManager::renderMenuBar() {
    if (ImGui::BeginMenuBar()) {
        // File menu
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Video...", "Ctrl+O")) {
                std::string filePath = openVideoFileDialog();
                if (!filePath.empty() && m_videoFileCallback) {
                    m_videoFileCallback(filePath);
                }
            }

            ImGui::Separator();

            if (ImGui::MenuItem("Exit", "Esc")) {
                // TODO: Signal application to exit
            }

            ImGui::EndMenu();
        }

        // Windows menu
        if (ImGui::BeginMenu("Windows")) {
            // Window visibility toggles
            for (auto& window : m_windows) {
                bool visible = window->isVisible();
                if (ImGui::MenuItem(window->getName(), nullptr, &visible)) {
                    window->setVisible(visible);
                }
            }

            ImGui::Separator();

            // Layout reset
            if (ImGui::MenuItem("Reset Layout")) {
                resetLayout();
            }

            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }
}

void WindowManager::createDefaultLayout(ImGuiID dockspaceId) {
    std::cout << "Creating default window layout..." << std::endl;

    // Clear any existing layout
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

    // Split the dockspace into regions:
    // - Bottom 30% for Timeline
    // - Top 70% split: Left 25% for Media Bin, Right 75% for Stage

    ImGuiID dockBottom = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Down, 0.3f, nullptr, &dockspaceId);
    ImGuiID dockLeft = ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.25f, nullptr, &dockspaceId);
    ImGuiID dockCenter = dockspaceId;  // Remaining center area is for Stage

    // Dock windows to their designated nodes
    ImGui::DockBuilderDockWindow("Timeline", dockBottom);
    ImGui::DockBuilderDockWindow("Media Bin", dockLeft);
    ImGui::DockBuilderDockWindow("Stage", dockCenter);

    ImGui::DockBuilderFinish(dockspaceId);

    std::cout << "Default layout created: Timeline (bottom), Media Bin (left), Stage (center)" << std::endl;
}

std::string WindowManager::openVideoFileDialog() {
#ifdef _WIN32
    // Windows native file dialog
    OPENFILENAMEA ofn;
    char szFile[260] = {0};

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Video Files\0*.mov;*.mp4;*.avi;*.mkv;*.webm\0"
                      "PNG Sequence\0*.png\0"
                      "All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = NULL;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = NULL;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameA(&ofn) == TRUE) {
        std::cout << "Selected file: " << ofn.lpstrFile << std::endl;
        return std::string(ofn.lpstrFile);
    }

    return "";  // User cancelled
#else
    std::cerr << "File dialog not implemented for this platform" << std::endl;
    return "";
#endif
}

} // namespace entity
