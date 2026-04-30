#pragma once

/**
 * Filename-based media versioning helpers.
 *
 * Files matching `<base>_v<tag>.<ext>` are considered versions of the same
 * logical media. The base (everything before `_v`) is the group key; the
 * tag is the version label. Resolution rules live in ProjectManager:
 *
 *   - clip.filepath without a `_v` tag → auto-roll: resolver picks the
 *     latest version in the group.
 *   - clip.filepath with a `_v` tag → pinned: resolver requires exact match.
 *
 * Conventions (fixed, not configurable):
 *   - Marker `_v` is case-insensitive (`_V01` matches the same as `_v01`).
 *   - Tag is alphanumeric only (`[A-Za-z0-9]+`). `_v01_alpha.mov` is NOT
 *     a versioned file — the whole stem is the base. Role-suffixed files
 *     get their own group, the safer default when a role taxonomy is
 *     unspecified.
 *   - Smart sort: numeric tags compare numerically (`v2 < v10`); otherwise
 *     lexicographic (handles date-style `v20210602a`).
 *
 * Pure functions, no project / filesystem dependencies. Unit tested in
 * `tests/unit/MediaVersioningTests.cpp`.
 */

#include <string>
#include <string_view>

namespace entity {

struct ParsedVersion {
    std::string base;  // The stem with the `_v<tag>` suffix removed.
    std::string tag;   // The version tag string. Empty if no `_v` suffix.
};

/**
 * Parse a filename stem (no extension) into base + tag. If `stem` does
 * not match `^(.+)_v([A-Za-z0-9]+)$`, returns `{stem, ""}`.
 */
ParsedVersion parseVersion(std::string_view stem);

/**
 * Compare two version tags. Returns <0, 0, or >0 like strcmp.
 *
 * If both tags parse cleanly as base-10 unsigned integers, compares
 * numerically (so `v02 < v10` even though lexicographically `v10 < v02`).
 * Otherwise compares lexicographically (handles date-style tags).
 *
 * Empty tags sort BEFORE any non-empty tag, so an unversioned file is
 * the "earliest" member of its group.
 */
int compareVersionTags(std::string_view a, std::string_view b);

/**
 * Compute the group key for a stored media path (project-relative for
 * Managed entries, absolute for Linked). The key is the directory plus
 * the version-stripped stem; extension is intentionally excluded so
 * `intro_v01.mov` and `intro_v01.mp4` group together (rare but
 * deliberate — same logical media in two containers).
 *
 * Linked and Managed entries with the same base name do NOT cross-group;
 * the caller is expected to scope group enumeration by pathKind. (This
 * helper does not see pathKind; it's purely string mechanics.)
 */
std::string groupKeyOf(std::string_view storedPath);

/**
 * Strip the `_v<tag>` suffix from a stored media path, returning what
 * the path would look like in auto-roll mode. `intro_v01.mov` →
 * `intro.mov`. Idempotent for already-unversioned paths.
 */
std::string toLogicalPath(std::string_view storedPath);

}  // namespace entity
