#include "entity/ui/WorkspaceStore.hpp"
#include "entity/core/AtomicFile.hpp"

#include <nlohmann/json.hpp>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

namespace entity {
namespace WorkspaceStore {

namespace {

// Mirrors Settings.cpp's appDataRoot(): %APPDATA%/Entity on Windows, with
// platform-appropriate paths elsewhere. Duplicated rather than shared
// because Settings.cpp's helper is in an anonymous namespace and the
// duplication is two short blocks; not worth a new utility header just yet.
std::filesystem::path appDataRoot() {
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        return std::filesystem::path(appdata) / "Entity";
    }
    if (const char* userProfile = std::getenv("USERPROFILE")) {
        return std::filesystem::path(userProfile) / "AppData" / "Roaming" / "Entity";
    }
    return std::filesystem::path(".") / ".entity";
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / "Library" / "Application Support" / "Entity";
    }
    return std::filesystem::path(".") / ".entity";
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
        return std::filesystem::path(xdg) / "Entity";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".config" / "Entity";
    }
    return std::filesystem::path(".") / ".entity";
#endif
}

constexpr uint32_t kWorkspacesVersion = 1;

} // namespace

std::filesystem::path workspacesPath() {
    return appDataRoot() / "workspaces.json";
}

Workspace defaultWorkspace() {
    Workspace w;
    w.name          = "Default";
    w.imguiIni      = "";  // empty triggers procedural createDefaultLayout()
    w.layoutLocked  = false;
    w.builtIn       = true;
    return w;
}

std::vector<Workspace> loadAll() {
    return loadAll(workspacesPath());
}

std::vector<Workspace> loadAll(const std::filesystem::path& path) {
    std::vector<Workspace> result;

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        // First run — seed with the built-in Default workspace.
        result.push_back(defaultWorkspace());
        return result;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        std::cerr << "[Workspaces] Could not open " << path
                  << " — falling back to Default" << std::endl;
        result.push_back(defaultWorkspace());
        return result;
    }

    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        std::cerr << "[Workspaces] Corrupt JSON at " << path
                  << " (" << e.what() << ") — falling back to Default" << std::endl;
        result.push_back(defaultWorkspace());
        return result;
    }

    // Defensive parse — every field is optional and falls back to default
    // on missing/wrong type, matching Settings.cpp's load behavior.
    if (j.contains("workspaces") && j["workspaces"].is_array()) {
        for (const auto& wj : j["workspaces"]) {
            Workspace w;
            if (wj.contains("name") && wj["name"].is_string()) {
                w.name = wj["name"].get<std::string>();
            }
            if (w.name.empty()) continue;  // can't address a nameless workspace
            if (wj.contains("imguiIni") && wj["imguiIni"].is_string()) {
                w.imguiIni = wj["imguiIni"].get<std::string>();
            }
            if (wj.contains("layoutLocked") && wj["layoutLocked"].is_boolean()) {
                w.layoutLocked = wj["layoutLocked"].get<bool>();
            }
            if (wj.contains("builtIn") && wj["builtIn"].is_boolean()) {
                w.builtIn = wj["builtIn"].get<bool>();
            }
            if (wj.contains("hiddenWindows") && wj["hiddenWindows"].is_array()) {
                for (const auto& hw : wj["hiddenWindows"]) {
                    if (hw.is_string()) w.hiddenWindows.push_back(hw.get<std::string>());
                }
            }
            // Phase 11 — per-node active tabs. Missing on pre-Phase-11
            // workspaces (focus pass no-ops, the ini selection stands).
            if (wj.contains("focusedTabs") && wj["focusedTabs"].is_array()) {
                for (const auto& ft : wj["focusedTabs"]) {
                    if (ft.is_string()) w.focusedTabs.push_back(ft.get<std::string>());
                }
            }
            result.push_back(std::move(w));
        }
    }

    // Always guarantee a Default exists. It may have been deleted manually
    // from the JSON, but the editor relies on its presence as a fallback.
    bool hasDefault = false;
    for (const auto& w : result) {
        if (w.name == "Default") { hasDefault = true; break; }
    }
    if (!hasDefault) {
        result.insert(result.begin(), defaultWorkspace());
    }

    return result;
}

bool saveAll(const std::vector<Workspace>& workspaces) {
    return saveAll(workspaces, workspacesPath());
}

bool saveAll(const std::vector<Workspace>& workspaces,
             const std::filesystem::path& path) {
    const auto dir = path.parent_path();

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::cerr << "[Workspaces] Could not create dir " << dir
                  << " (" << ec.message() << ")" << std::endl;
        return false;
    }

    json j;
    j["version"] = kWorkspacesVersion;
    j["workspaces"] = json::array();
    for (const auto& w : workspaces) {
        json wj;
        wj["name"]          = w.name;
        wj["imguiIni"]      = w.imguiIni;
        wj["layoutLocked"]  = w.layoutLocked;
        wj["builtIn"]       = w.builtIn;
        wj["hiddenWindows"] = w.hiddenWindows;
        wj["focusedTabs"]   = w.focusedTabs;  // Phase 11 — per-node active tabs
        j["workspaces"].push_back(std::move(wj));
    }

    // Write-temp-then-rename so a crash mid-write can never corrupt the
    // prior workspaces file. See entity/core/AtomicFile.hpp.
    std::string err;
    if (!writeFileAtomic(path, j.dump(2), &err)) {
        std::cerr << "[Workspaces] " << err << std::endl;
        return false;
    }
    return true;
}

} // namespace WorkspaceStore
} // namespace entity
