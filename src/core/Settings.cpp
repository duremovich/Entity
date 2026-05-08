#include "entity/core/Settings.hpp"

#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <cstdlib>
#include <mutex>

#ifdef _WIN32
#include <cstring>
#endif

using json = nlohmann::json;

namespace entity {

namespace {

// On Windows, %APPDATA% resolves to the user's roaming profile, e.g.
// C:\Users\<name>\AppData\Roaming. We append "Entity" as our app dir.
// macOS / Linux paths are placeholders until those builds are first attempted.
std::filesystem::path appDataRoot() {
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        return std::filesystem::path(appdata) / "Entity";
    }
    // Fallback: USERPROFILE/AppData/Roaming/Entity
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

constexpr uint32_t kSettingsVersion = 1;

} // namespace

std::filesystem::path settingsPath() {
    return appDataRoot() / "settings.json";
}

Settings loadSettings() {
    return loadSettings(settingsPath());
}

Settings loadSettings(const std::filesystem::path& path) {
    Settings settings; // defaults

    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return settings; // first run — use defaults silently
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        std::cerr << "[Settings] Could not open " << path
                  << " — falling back to defaults" << std::endl;
        return settings;
    }

    json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        std::cerr << "[Settings] Corrupt JSON at " << path
                  << " (" << e.what() << ") — falling back to defaults" << std::endl;
        return settings;
    }

    // Each field is read defensively — a missing or wrong-typed key falls
    // back to the default. This makes forward/backward compatibility free:
    // older files load with new fields at default; newer files load on older
    // builds with unknown fields ignored.
    if (j.contains("frameCacheBytes") && j["frameCacheBytes"].is_number_unsigned()) {
        settings.frameCacheBytes = j["frameCacheBytes"].get<uint64_t>();
    }

    // Phase C.12 #7 — OCIO color management. Strings, all optional.
    auto readString = [&](const char* key, std::string& dst) {
        if (j.contains(key) && j[key].is_string()) {
            dst = j[key].get<std::string>();
        }
    };
    readString("ocioConfigPath",      settings.ocioConfigPath);
    readString("defaultVideoInputCs", settings.defaultVideoInputCs);
    readString("defaultPngInputCs",   settings.defaultPngInputCs);
    readString("defaultDisplay",      settings.defaultDisplay);
    readString("defaultView",         settings.defaultView);

    if (j.contains("importStoragePolicy") && j["importStoragePolicy"].is_number_integer()) {
        const int raw = j["importStoragePolicy"].get<int>();
        if (raw >= 0 && raw <= 2) {
            settings.importStoragePolicy =
                static_cast<Settings::ImportStoragePolicy>(raw);
        }
    }

    if (j.contains("oscReceiverEnabled") && j["oscReceiverEnabled"].is_boolean()) {
        settings.oscReceiverEnabled = j["oscReceiverEnabled"].get<bool>();
    }
    if (j.contains("oscReceiverPort") && j["oscReceiverPort"].is_number_unsigned()) {
        const auto raw = j["oscReceiverPort"].get<uint64_t>();
        if (raw > 0 && raw < 65536) {
            settings.oscReceiverPort = static_cast<uint16_t>(raw);
        }
    }

    return settings;
}

bool saveSettings(const Settings& settings) {
    return saveSettings(settings, settingsPath());
}

bool saveSettings(const Settings& settings, const std::filesystem::path& path) {
    const auto dir = path.parent_path();

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::cerr << "[Settings] Could not create settings dir " << dir
                  << " (" << ec.message() << ")" << std::endl;
        return false;
    }

    json j;
    j["version"] = kSettingsVersion;
    j["frameCacheBytes"]      = settings.frameCacheBytes;
    j["ocioConfigPath"]       = settings.ocioConfigPath;
    j["defaultVideoInputCs"]  = settings.defaultVideoInputCs;
    j["defaultPngInputCs"]    = settings.defaultPngInputCs;
    j["defaultDisplay"]       = settings.defaultDisplay;
    j["defaultView"]          = settings.defaultView;
    j["importStoragePolicy"]  = static_cast<int>(settings.importStoragePolicy);
    j["oscReceiverEnabled"]   = settings.oscReceiverEnabled;
    j["oscReceiverPort"]      = settings.oscReceiverPort;

    // Write to a temp file then rename — atomic on POSIX, near-atomic on
    // Windows (rename-over-existing requires MoveFileEx, but
    // std::filesystem::rename uses ReplaceFile on modern MSVC). Either way
    // a crash mid-write doesn't corrupt the prior settings.
    const auto tmpPath = path.string() + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[Settings] Could not write " << tmpPath << std::endl;
            return false;
        }
        out << j.dump(2);
        if (!out.good()) {
            std::cerr << "[Settings] Stream error writing " << tmpPath << std::endl;
            return false;
        }
    }

    std::filesystem::rename(tmpPath, path, ec);
    if (ec) {
        std::cerr << "[Settings] Rename " << tmpPath << " -> " << path
                  << " failed (" << ec.message() << ")" << std::endl;
        std::filesystem::remove(tmpPath); // best-effort cleanup
        return false;
    }
    return true;
}

namespace {
// Process-wide snapshot. Worker threads (decoders) read this; Engine writes
// it once at startup and again on Preferences "OK". Mutex-guarded copy is
// fine — read frequency is at most decode-rate, write frequency is at most
// once per user-visible config change.
std::mutex g_activeSettingsMutex;
Settings   g_activeSettings;
}

Settings activeSettings() {
    std::lock_guard<std::mutex> lock(g_activeSettingsMutex);
    return g_activeSettings;
}

void publishActiveSettings(const Settings& s) {
    std::lock_guard<std::mutex> lock(g_activeSettingsMutex);
    g_activeSettings = s;
}

} // namespace entity
