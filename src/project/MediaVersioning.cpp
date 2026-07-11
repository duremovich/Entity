#include "entity/project/MediaVersioning.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

namespace entity {

namespace {

std::size_t leadingDigitCount(std::string_view s) {
    std::size_t n = 0;
    while (n < s.size() && s[n] >= '0' && s[n] <= '9') ++n;
    return n;
}

// Compare two non-empty digit runs as arbitrary-precision unsigned
// integers (no conversion, so no overflow): strip leading zeros, then a
// longer run is greater, then lexicographic settles equal lengths.
// "1" and "01" compare equal.
int compareDigitRuns(std::string_view a, std::string_view b) {
    a.remove_prefix(std::min(a.find_first_not_of('0'), a.size()));
    b.remove_prefix(std::min(b.find_first_not_of('0'), b.size()));
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    if (a < b) return -1;
    if (a > b) return  1;
    return 0;
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
    // Pattern: ^(.+)_[vV]([0-9][A-Za-z0-9]*)$ — anchored to the end of stem.
    // The tag must start with a digit so that word suffixes containing a
    // `_v` (`intro_visual`, `title_video`, `cut_version`) are not mistaken
    // for version tags and mis-grouped with their prefix (`intro.mov`).
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
    if (tagCandidate.empty() ||
        tagCandidate.front() < '0' || tagCandidate.front() > '9' ||
        !allAlnum(tagCandidate)) {
        return {std::string(stem), {}};
    }
    return {std::string(stem.substr(0, marker)), std::string(tagCandidate)};
}

int compareVersionTags(std::string_view a, std::string_view b) {
    if (a.empty() && b.empty()) return 0;
    if (a.empty()) return -1;
    if (b.empty()) return  1;

    const std::size_t da = leadingDigitCount(a);
    const std::size_t db = leadingDigitCount(b);
    if (da > 0 && db > 0) {
        // Numeric on the leading digits, so "2" < "10" and "2a" < "10a";
        // an alnum suffix breaks ties lexicographically ("2a" < "2b",
        // and plain "2" < suffixed "2a").
        const int num = compareDigitRuns(a.substr(0, da), b.substr(0, db));
        if (num != 0) return num;
        a.remove_prefix(da);
        b.remove_prefix(db);
    }
    // Lexicographic fallback (defensive — parseVersion only emits
    // digit-leading tags, so both runs above are normally non-empty).
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
