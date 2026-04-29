#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace entity {

// Strong-typed identifier for media assets that travel between Director
// and Renderer over the bus. The underlying value is a `std::string` --
// today a filesystem path; in a future content-hash migration the same
// shape can carry an opaque hash without breaking call sites.
//
// Strong typedef rules: explicit constructors, no implicit string->AssetId
// conversions, no implicit AssetId->string conversions. The point is to
// catch the moment the wire format ever needs to switch.
//
// Phase D entry, subtask 9: today only `bus::ProvisionClipResources`
// names assets via this type. Other call sites
// (`Clip.filepath`, `MediaLibraryEntry.originalPath`,
// `ProjectSerializer`, scripts/*.json) keep their `std::string` paths
// unchanged -- the typedef seam is positioned where the future hash
// migration will land first.
class AssetId {
public:
    AssetId() = default;
    explicit AssetId(std::string value) noexcept
        : m_value(std::move(value)) {}
    explicit AssetId(std::string_view value)
        : m_value(value) {}
    explicit AssetId(const char* value)
        : m_value(value ? value : "") {}

    AssetId(const AssetId&) = default;
    AssetId(AssetId&&) noexcept = default;
    AssetId& operator=(const AssetId&) = default;
    AssetId& operator=(AssetId&&) noexcept = default;
    ~AssetId() = default;

    // Underlying access. Marked `value()` rather than implicit
    // `operator std::string()` so call sites that need the raw bytes
    // are explicit -- mirrors the std::filesystem::path / std::string
    // boundary discipline elsewhere in the codebase.
    const std::string& value() const noexcept { return m_value; }
    std::string&       value() noexcept       { return m_value; }
    bool empty() const noexcept { return m_value.empty(); }

    // Equality / hashing for use as keys in unordered containers.
    friend bool operator==(const AssetId& lhs, const AssetId& rhs) noexcept {
        return lhs.m_value == rhs.m_value;
    }
    friend bool operator!=(const AssetId& lhs, const AssetId& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    std::string m_value;
};

// Path-canonicalize: normalize separators to '/'. Mirrors what the bus
// would expect once Director and Renderer are on different machines and
// raw OS paths can't cross the wire. Idempotent. Empty stays empty.
//
// We deliberately do NOT lower-case (case-sensitivity varies per
// filesystem; mangling here would break Linux), expand `~`, or resolve
// symlinks (filesystem state isn't deterministic). When the migration
// to content-hash happens, this becomes the seam where the path
// resolves to a hash by lookup.
inline AssetId canonicalize(const AssetId& id) {
    std::string out = id.value();
    for (char& c : out) {
        if (c == '\\') c = '/';
    }
    return AssetId{std::move(out)};
}

inline AssetId canonicalize(std::string_view raw) {
    return canonicalize(AssetId{raw});
}

} // namespace entity

namespace std {
template <>
struct hash<entity::AssetId> {
    std::size_t operator()(const entity::AssetId& id) const noexcept {
        return std::hash<std::string>{}(id.value());
    }
};
} // namespace std
