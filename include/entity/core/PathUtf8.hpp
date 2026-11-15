#pragma once

/**
 * UTF-8 <-> std::filesystem::path helpers.
 *
 * On MSVC, path::string() / generic_string() return bytes in the system ANSI
 * code page and throw std::system_error for characters outside it. Strings
 * that are persisted (JSON), sent over the bus, or logged on open/save/scan
 * paths must go through these helpers instead. Project files and
 * recent_projects.json store UTF-8.
 *
 * The editor also embeds a manifest that sets activeCodePage=UTF-8 (Windows
 * 10 1903+), but these helpers do not depend on it.
 */

#include <filesystem>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace entity {

/// Path -> UTF-8 string (native separators). Never throws for valid paths.
inline std::string pathToUtf8(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

/// Path -> UTF-8 string with forward slashes.
inline std::string pathToUtf8Generic(const std::filesystem::path& p) {
    auto u8 = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

/// UTF-8 string -> path. Falls back to the narrow-codepage constructor if the
/// input is not valid UTF-8 (e.g. a legacy project saved with ANSI bytes).
inline std::filesystem::path utf8ToPath(const std::string& utf8) {
    if (utf8.empty()) return {};
#ifdef _WIN32
    const int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) {
        // Not valid UTF-8: legacy ANSI bytes. Decode with the ANSI code page.
        const int alen = MultiByteToWideChar(CP_ACP, 0, utf8.c_str(),
                                             static_cast<int>(utf8.size()), nullptr, 0);
        if (alen <= 0) return {};
        std::wstring w(static_cast<size_t>(alen), L'\0');
        MultiByteToWideChar(CP_ACP, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                            w.data(), alen);
        return std::filesystem::path(w);
    }
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(),
                        static_cast<int>(utf8.size()), w.data(), wlen);
    return std::filesystem::path(w);
#else
    return std::filesystem::path(utf8);
#endif
}

}  // namespace entity
