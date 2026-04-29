#pragma once

/**
 * RecentProjects — persisted list of recently-opened .entity files.
 *
 * ADR-0009 / Phase D structured-projects epic: the Project Launcher's
 * "Recent" panel reads from here. When the user opens or creates a project
 * via the launcher (or via File > Open / File > New from inside the editor),
 * the path gets bumped to the front of the list; older duplicates are
 * dedup'd; the list is capped at kMaxEntries.
 *
 * Storage: `<userAppData>/Entity/recent_projects.json`. On Windows that's
 * `%APPDATA%/Entity/recent_projects.json` (CSIDL_APPDATA / SHGetFolderPath).
 * On other platforms the storage path is XDG-style under $HOME/.config/
 * — a future-proofing detail; non-Windows platforms aren't shipped yet,
 * but the path-resolution helper is platform-conditional so adding macOS
 * / Linux later doesn't churn callers.
 *
 * Schema is intentionally tiny — a JSON array of objects with `path` and
 * `lastOpenedUnixSeconds`. No version field; if the file is malformed we
 * silently start fresh rather than fail loudly. The launcher must be
 * robust to a missing/corrupt recent-projects file (first-run state, edited
 * by hand, partial write from a crash). The cost of being wrong is "you
 * see an empty Recent list," not data loss.
 *
 * Threading: this class is not thread-safe. Calls happen from the main
 * thread (UI loop / project load path).
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace entity {

class RecentProjects {
public:
    static constexpr std::size_t kMaxEntries = 10;

    struct Entry {
        std::string path;                 // UTF-8 absolute path to the .entity file
        std::int64_t lastOpenedUnixSeconds{0};
    };

    /**
     * Resolve the storage path the load/save methods use. Exposed so tests
     * can verify the location without invoking platform APIs.
     */
    static std::filesystem::path storagePath();

    /**
     * Load entries from `storagePath()`. Returns an empty list if the file
     * is missing or malformed — no error reporting, the launcher just
     * shows an empty Recent panel in that case.
     */
    void load();

    /**
     * Load entries from a specific file. Used by tests to avoid touching
     * the user's actual recent-projects file.
     */
    void loadFrom(const std::filesystem::path& file);

    /**
     * Persist entries to `storagePath()`. Creates the parent directory if
     * needed. Returns false on filesystem error; true otherwise (including
     * the trivial empty-list write).
     */
    bool save() const;

    /**
     * Persist entries to a specific file. Used by tests.
     */
    bool saveTo(const std::filesystem::path& file) const;

    /**
     * Bump `path` to the front of the list with a "now" timestamp,
     * dedup'd against existing entries (same canonical path collapses to
     * one entry, position freshened). Trims the tail beyond kMaxEntries.
     *
     * Empty paths are rejected (no-op).
     */
    void touch(const std::string& path);

    /**
     * Remove an entry by path (e.g. when the user clicks "Forget" or the
     * launcher detects a missing-folder entry). No-op if absent.
     */
    void remove(const std::string& path);

    /**
     * Clear everything. Convenience for tests; also exposed in case the
     * launcher gets a "Clear Recent" action later.
     */
    void clear() { m_entries.clear(); }

    const std::vector<Entry>& entries() const { return m_entries; }
    bool                      empty()   const { return m_entries.empty(); }
    std::size_t               size()    const { return m_entries.size(); }

private:
    std::vector<Entry> m_entries;
};

} // namespace entity
