#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace entity {

// Durable file write: write to "<path>.tmp", flush, verify the stream,
// then rename into place (atomic on NTFS and POSIX — MSVC's
// std::filesystem::rename replaces an existing destination). The
// destination always holds either its previous content or the complete
// new content, never a truncated mix, so a crash / power loss / full
// disk mid-save cannot destroy the prior good file.
//
// On failure: returns false, best-effort removes the temp file, leaves
// the destination untouched, and (if err is non-null) writes a
// human-readable reason into *err.
//
// Single point of truth for this pattern — used by ProjectSerializer,
// RecentProjects, Settings, and WorkspaceStore. Future durability work
// (e.g. FlushFileBuffers before the rename) belongs here, once.
inline bool writeFileAtomic(const std::filesystem::path& path,
                            std::string_view contents,
                            std::string* err = nullptr) {
    namespace fs = std::filesystem;
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            if (err) *err = "Failed to open file for writing: " + tmp.string();
            return false;
        }
        out.write(contents.data(),
                  static_cast<std::streamsize>(contents.size()));
        out.flush();
        if (!out.good()) {
            if (err) *err = "Write failed (disk full?): " + tmp.string();
            out.close();
            std::error_code removeEc;
            fs::remove(tmp, removeEc);
            return false;
        }
    }
    std::error_code renameEc;
    fs::rename(tmp, path, renameEc);
    if (renameEc) {
        if (err) {
            *err = "Failed to move " + tmp.string() + " into place: "
                 + renameEc.message();
        }
        std::error_code removeEc;
        fs::remove(tmp, removeEc);
        return false;
    }
    return true;
}

} // namespace entity
