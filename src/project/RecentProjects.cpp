/**
 * RecentProjects implementation. See header for the rationale.
 */

#include "entity/project/RecentProjects.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#endif

namespace fs = std::filesystem;

namespace entity {

namespace {

fs::path appDataRoot() {
#ifdef _WIN32
    // SHGetFolderPathW(CSIDL_APPDATA) returns the user's roaming AppData
    // directory ("C:/Users/<name>/AppData/Roaming"). We prefer this over
    // %APPDATA% env vars so a user with a customized environment block
    // still gets the right path.
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) {
        return fs::path(buf);
    }
    return {};
#else
    // XDG Base Directory: $XDG_CONFIG_HOME if set, else $HOME/.config.
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return fs::path(xdg);
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return fs::path(home) / ".config";
    }
    return {};
#endif
}

std::int64_t nowUnixSeconds() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

}  // namespace

fs::path RecentProjects::storagePath() {
    auto root = appDataRoot();
    if (root.empty()) return {};
    return root / "Entity" / "recent_projects.json";
}

void RecentProjects::load() {
    loadFrom(storagePath());
}

void RecentProjects::loadFrom(const fs::path& file) {
    m_entries.clear();
    if (file.empty()) return;

    std::error_code ec;
    if (!fs::exists(file, ec)) return;

    std::ifstream in(file);
    if (!in.is_open()) return;

    nlohmann::json doc;
    try {
        in >> doc;
    } catch (const nlohmann::json::exception&) {
        return;  // Corrupt file -> fresh start.
    }

    if (!doc.is_array()) return;

    m_entries.reserve(doc.size());
    for (const auto& item : doc) {
        if (!item.is_object()) continue;
        Entry e;
        if (auto p = item.find("path"); p != item.end() && p->is_string()) {
            e.path = p->get<std::string>();
        }
        if (auto t = item.find("lastOpenedUnixSeconds"); t != item.end() && t->is_number_integer()) {
            e.lastOpenedUnixSeconds = t->get<std::int64_t>();
        }
        if (e.path.empty()) continue;
        m_entries.push_back(std::move(e));
        if (m_entries.size() >= kMaxEntries) break;
    }
}

bool RecentProjects::save() const {
    return saveTo(storagePath());
}

bool RecentProjects::saveTo(const fs::path& file) const {
    if (file.empty()) return false;

    if (file.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        if (ec) {
            std::cerr << "[RecentProjects] Failed to create "
                      << file.parent_path().string() << ": " << ec.message() << std::endl;
            return false;
        }
    }

    nlohmann::json doc = nlohmann::json::array();
    for (const auto& e : m_entries) {
        doc.push_back({
            {"path",                  e.path},
            {"lastOpenedUnixSeconds", e.lastOpenedUnixSeconds},
        });
    }

    // Write-temp-then-rename, same as ProjectSerializer::save — never
    // truncate the previous good file before the new content is durable.
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[RecentProjects] Failed to open " << tmp.string() << " for write" << std::endl;
            return false;
        }
        out << doc.dump(2);
        out.flush();
        if (!out.good()) {
            std::error_code ec;
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        std::cerr << "[RecentProjects] Failed to move " << tmp.string()
                  << " into place: " << ec.message() << std::endl;
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void RecentProjects::touch(const std::string& path) {
    if (path.empty()) return;

    m_entries.erase(
        std::remove_if(m_entries.begin(), m_entries.end(),
            [&](const Entry& e) { return e.path == path; }),
        m_entries.end());

    Entry fresh;
    fresh.path = path;
    fresh.lastOpenedUnixSeconds = nowUnixSeconds();
    m_entries.insert(m_entries.begin(), std::move(fresh));

    if (m_entries.size() > kMaxEntries) {
        m_entries.resize(kMaxEntries);
    }
}

void RecentProjects::remove(const std::string& path) {
    m_entries.erase(
        std::remove_if(m_entries.begin(), m_entries.end(),
            [&](const Entry& e) { return e.path == path; }),
        m_entries.end());
}

}  // namespace entity
