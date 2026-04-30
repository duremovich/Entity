#include "entity/project/MediaVersioning.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>

namespace entity {

namespace {

bool allDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

bool allAlnum(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc)) return false;
    }
    return true;
}

// Split a stored path into (parent, stem, ext). Parent includes a trailing
// slash if non-empty; ext includes the leading dot if non-empty.
struct PathParts {
    std::string_view parent;
    std::string_view stem;
    std::string_view ext;
};

PathParts splitPath(std::string_view path) {
    PathParts out;
    // Find the last separator (forward or back slash) to isolate the filename.
    std::size_t sep = path.find_last_of("/\\");
    std::string_view filename;
    if (sep == std::string_view::npos) {
        filename     = path;
        out.parent   = {};
    } else {
        filename     = path.substr(sep + 1);
        out.parent   = path.substr(0, sep + 1);  // includes the separator
    }
    // Find the last dot in the filename (not the path) to split stem/ext.
    std::size_t dot = filename.find_last_of('.');
    if (dot == std::string_view::npos) {
        out.stem = filename;
        out.ext  = {};
    } else {
        out.stem = filename.substr(0, dot);
        out.ext  = filename.substr(dot);
    }
    return out;
}

}  // namespace

ParsedVersion parseVersion(std::string_view stem) {
    // Pattern: ^(.+)_[vV]([A-Za-z0-9]+)$ — anchored to the end of stem.
    // Lowercase a copy so the rfind is case-insensitive on the marker;
    // the tag is extracted from the original to preserve any user
    // casing (display normalizes anyway).
    std::string lower(stem);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    const std::size_t marker = lower.rfind("_v");
    if (marker == std::string::npos || marker == 0) {
        return {std::string(stem), {}};
    }
    std::string_view tagCandidate = stem.substr(marker + 2);
    if (!allAlnum(tagCandidate)) {
        return {std::string(stem), {}};
    }
    return {std::string(stem.substr(0, marker)), std::string(tagCandidate)};
}

int compareVersionTags(std::string_view a, std::string_view b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return -1;
    if (b.empty()) return  1;

    if (allDigits(a) && allDigits(b)) {
        // Compare as unsigned integers. Use uint64_t — version tags
        // realistically never overflow, but be defensive.
        unsigned long long ai = 0, bi = 0;
        std::from_chars(a.data(), a.data() + a.size(), ai);
        std::from_chars(b.data(), b.data() + b.size(), bi);
        if (ai < bi) return -1;
        if (ai > bi) return  1;
        return 0;
    }
    // Lexicographic fallback.
    if (a < b) return -1;
    if (a > b) return  1;
    return 0;
}

std::string groupKeyOf(std::string_view storedPath) {
    PathParts parts = splitPath(storedPath);
    ParsedVersion v = parseVersion(parts.stem);
    // groupKey = parent + base (no extension, no version tag).
    std::string out;
    out.reserve(parts.parent.size() + v.base.size());
    out.append(parts.parent);
    out.append(v.base);
    return out;
}

std::string toLogicalPath(std::string_view storedPath) {
    PathParts parts = splitPath(storedPath);
    ParsedVersion v = parseVersion(parts.stem);
    if (v.tag.empty()) {
        return std::string(storedPath);  // already logical
    }
    std::string out;
    out.reserve(parts.parent.size() + v.base.size() + parts.ext.size());
    out.append(parts.parent);
    out.append(v.base);
    out.append(parts.ext);
    return out;
}

}  // namespace entity
