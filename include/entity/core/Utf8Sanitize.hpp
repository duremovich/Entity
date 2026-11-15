#pragma once

/**
 * UTF-8 validation / repair for text that arrives from outside the editor
 * (OSC strings, plugin settings) and ends up in project state.
 *
 * nlohmann::json::dump() throws on invalid UTF-8 (strict error handler),
 * and ProjectSerializer::save turns that into a failed save. One bad byte
 * in an OSC text argument copied into a Text layer would make every save
 * and autosave fail until the layer was edited by hand. Sanitizing at the
 * point the text enters keeps project state always serializable.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace entity {

namespace detail {

// Length of the well-formed sequence starting at s[i], or 0 if ill-formed.
// `maximalSubpart` receives how many bytes to skip for one U+FFFD.
inline std::size_t utf8SequenceLength(std::string_view s, std::size_t i,
                                      std::size_t& maximalSubpart) noexcept {
    const auto b0 = static_cast<std::uint8_t>(s[i]);
    maximalSubpart = 1;
    if (b0 < 0x80) return 1;
    std::size_t need = 0;
    std::uint8_t lo = 0x80, hi = 0xBF;  // allowed range for the 2nd byte
    if (b0 >= 0xC2 && b0 <= 0xDF)      { need = 1; }
    else if (b0 == 0xE0)               { need = 2; lo = 0xA0; }
    else if (b0 >= 0xE1 && b0 <= 0xEC) { need = 2; }
    else if (b0 == 0xED)               { need = 2; hi = 0x9F; }   // no surrogates
    else if (b0 >= 0xEE && b0 <= 0xEF) { need = 2; }
    else if (b0 == 0xF0)               { need = 3; lo = 0x90; }
    else if (b0 >= 0xF1 && b0 <= 0xF3) { need = 3; }
    else if (b0 == 0xF4)               { need = 3; hi = 0x8F; }   // <= U+10FFFF
    else return 0;                                                // C0, C1, F5..FF, stray continuation
    for (std::size_t k = 1; k <= need; ++k) {
        if (i + k >= s.size()) return 0;
        const auto b = static_cast<std::uint8_t>(s[i + k]);
        const std::uint8_t l = (k == 1) ? lo : 0x80;
        const std::uint8_t h = (k == 1) ? hi : 0xBF;
        if (b < l || b > h) return 0;
        maximalSubpart = k + 1;
    }
    return need + 1;
}

} // namespace detail

/// True when `s` is well-formed UTF-8 (RFC 3629: no overlongs, no
/// surrogates, nothing above U+10FFFF).
inline bool isValidUtf8(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t skip = 1;
        const std::size_t len = detail::utf8SequenceLength(s, i, skip);
        if (len == 0) return false;
        i += len;
    }
    return true;
}

/// Copy of `s` with every ill-formed sequence replaced by U+FFFD (one
/// replacement per maximal invalid subpart, as Unicode recommends).
/// Valid input is returned unchanged.
inline std::string sanitizeUtf8(std::string_view s) {
    if (isValidUtf8(s)) return std::string(s);
    std::string out;
    out.reserve(s.size() + 8);
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t skip = 1;
        const std::size_t len = detail::utf8SequenceLength(s, i, skip);
        if (len == 0) {
            out += "\xEF\xBF\xBD";  // U+FFFD REPLACEMENT CHARACTER
            i += skip;
        } else {
            out.append(s.data() + i, len);
            i += len;
        }
    }
    return out;
}

} // namespace entity
