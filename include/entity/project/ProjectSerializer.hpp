#pragma once

#include "entity/timeline/Timeline.hpp"
#include <string>
#include <filesystem>

namespace entity {

/**
 * ProjectSerializer - Save and load Entity project files.
 *
 * Serializes timeline state, tracks, and clips to JSON format.
 * Runtime resources (textures, decoders) are NOT serialized;
 * they are recreated when a project is loaded.
 *
 * File format: .entity (JSON)
 */
class ProjectSerializer {
public:
    // Project file version for forward compatibility.
    //   v3 (2026-04-23) — adds Timeline::Section array.
    //   v2 — adds OutputDisplay + Models + Screens.
    //   v1 — original.
    // Loader is forward-compatible with older files: missing arrays no-op.
    static constexpr int PROJECT_VERSION = 3;
    static constexpr const char* FILE_EXTENSION = ".entity";

    /**
     * Save a timeline to a project file.
     * @param timeline The timeline to save
     * @param filepath Path to save to (will append .entity if needed)
     * @return True if save was successful
     */
    static bool save(const Timeline& timeline, const std::filesystem::path& filepath);

    /**
     * Load a project file into a timeline.
     * Clears existing timeline content before loading.
     * @param timeline The timeline to load into
     * @param filepath Path to the project file
     * @param mediaLoadCallback Callback to load media after clips are created
     * @return True if load was successful
     */
    using MediaLoadCallback = std::function<void(entt::entity clipEntity, const std::string& filepath)>;
    static bool load(Timeline& timeline, const std::filesystem::path& filepath,
                     MediaLoadCallback mediaLoadCallback = nullptr);

    /**
     * Get the last error message (if save/load failed).
     */
    static const std::string& getLastError() { return s_lastError; }

private:
    static std::string s_lastError;
};

} // namespace entity
