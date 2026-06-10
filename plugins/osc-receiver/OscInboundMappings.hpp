// SPDX-License-Identifier: Apache-2.0
// Inbound OSC route-table types + parse/match/expand helpers.
// No Winsock, no plugin-api — safe to include from unit tests.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osc_inbound {

// ---------------------------------------------------------------------------
// Types

struct CommandSpec {
    std::string commandType;
    std::string paramsTemplate;
};

struct InboundRoute {
    std::string              addressPattern;
    std::string              captureKey;
    std::vector<CommandSpec> commands;
};

// ---------------------------------------------------------------------------
// Wire-read helpers (big-endian)

inline std::uint32_t readU32BE(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8)  |  std::uint32_t(p[3]);
}
inline std::int32_t readI32BE(const std::uint8_t* p) {
    return static_cast<std::int32_t>(readU32BE(p));
}
inline float readF32BE(const std::uint8_t* p) {
    std::uint32_t bits = readU32BE(p);
    float f; std::memcpy(&f, &bits, sizeof(f)); return f;
}
inline std::int64_t readI64BE(const std::uint8_t* p) {
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | p[i];
    return static_cast<std::int64_t>(bits);
}
inline double readF64BE(const std::uint8_t* p) {
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | p[i];
    double d; std::memcpy(&d, &bits, sizeof(d)); return d;
}

// ---------------------------------------------------------------------------
// JSON whitespace

inline bool isJsonWs(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// ---------------------------------------------------------------------------
// Default route table

inline std::vector<InboundRoute> defaultRoutes() {
    std::vector<InboundRoute> routes;
    routes.push_back({"/entity/play",         "", {{"Play", ""}}});
    routes.push_back({"/entity/pause",        "", {{"Pause", ""}}});
    routes.push_back({"/entity/stop",         "", {{"Pause", ""}, {"SeekToFrame", "{\"frame\":0}"}}});
    routes.push_back({"/entity/section/next", "", {{"SectionGo", ""}}});
    routes.push_back({"/entity/seek",         "", {{"SeekToFrame", "{\"frame\":$arg0i}"}}});
    routes.push_back({"/entity/cue/{number}/go","number",{{"FireCue","{\"number\":$capturef}"}}});
    return routes;
}

// ---------------------------------------------------------------------------
// JSON parser — bare array → route table

// Read a JSON string starting at the opening '"'. Advances pos past closing '"'.
inline bool readJsonStr(std::string_view s, std::size_t& pos, std::string& out) {
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size() && s[pos] != '"') {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            ++pos;
            switch (s[pos]) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case 'n':  out.push_back('\n'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                default:   out.push_back(s[pos]); break;
            }
        } else { out.push_back(s[pos]); }
        ++pos;
    }
    if (pos >= s.size()) return false;
    ++pos;
    return true;
}

inline void extractCaptureKey(const std::string& pattern, std::string& key) {
    key.clear();
    auto open = pattern.find('{');
    if (open == std::string::npos) return;
    auto close = pattern.find('}', open + 1);
    if (close == std::string::npos) return;
    key = pattern.substr(open + 1, close - open - 1);
}

inline std::vector<InboundRoute> parseInboundMappingsJson(std::string_view json) {
    if (json.empty()) return defaultRoutes();

    auto skipWs = [](std::string_view s, std::size_t i) -> std::size_t {
        while (i < s.size() && isJsonWs(s[i])) ++i;
        return i;
    };

    std::size_t pos = skipWs(json, 0);
    if (pos >= json.size() || json[pos] != '[') return defaultRoutes();
    ++pos;

    std::vector<InboundRoute> routes;
    while (true) {
        pos = skipWs(json, pos);
        if (pos >= json.size() || json[pos] == ']') break;
        if (json[pos] == ',') { ++pos; continue; }
        if (json[pos] != '{') break;
        ++pos;

        InboundRoute route;
        while (true) {
            pos = skipWs(json, pos);
            if (pos >= json.size() || json[pos] == '}') break;
            if (json[pos] == ',') { ++pos; continue; }

            std::string key;
            if (!readJsonStr(json, pos, key)) break;
            pos = skipWs(json, pos);
            if (pos >= json.size() || json[pos] != ':') break;
            ++pos;
            pos = skipWs(json, pos);

            if (key == "address") {
                readJsonStr(json, pos, route.addressPattern);
            } else if (key == "captureKey") {
                readJsonStr(json, pos, route.captureKey);
            } else if (key == "commands") {
                if (pos >= json.size() || json[pos] != '[') break;
                ++pos;
                while (true) {
                    pos = skipWs(json, pos);
                    if (pos >= json.size() || json[pos] == ']') break;
                    if (json[pos] == ',') { ++pos; continue; }
                    if (json[pos] != '{') break;
                    ++pos;

                    CommandSpec cmd;
                    while (true) {
                        pos = skipWs(json, pos);
                        if (pos >= json.size() || json[pos] == '}') break;
                        if (json[pos] == ',') { ++pos; continue; }
                        std::string ck;
                        if (!readJsonStr(json, pos, ck)) break;
                        pos = skipWs(json, pos);
                        if (pos >= json.size() || json[pos] != ':') break;
                        ++pos;
                        pos = skipWs(json, pos);
                        std::string cv;
                        if (readJsonStr(json, pos, cv)) {
                            if (ck == "type")        cmd.commandType    = cv;
                            else if (ck == "params") cmd.paramsTemplate = cv;
                        }
                    }
                    if (pos < json.size() && json[pos] == '}') ++pos;
                    if (!cmd.commandType.empty()) route.commands.push_back(std::move(cmd));
                }
                if (pos < json.size() && json[pos] == ']') ++pos;
            } else {
                // skip unknown string value
                if (pos < json.size() && json[pos] == '"') {
                    std::string dummy; readJsonStr(json, pos, dummy);
                }
            }
        }
        if (pos < json.size() && json[pos] == '}') ++pos;

        if (!route.addressPattern.empty() && !route.commands.empty()) {
            if (route.captureKey.empty())
                extractCaptureKey(route.addressPattern, route.captureKey);
            routes.push_back(std::move(route));
        }
    }

    if (routes.empty()) return defaultRoutes();
    return routes;
}

// ---------------------------------------------------------------------------
// Pattern matching

inline bool matchPattern(std::string_view pattern, std::string_view address,
                          std::string_view& capture) {
    auto open = pattern.find('{');
    if (open == std::string_view::npos) {
        capture = {};
        return pattern == address;
    }
    auto close = pattern.find('}', open + 1);
    if (close == std::string_view::npos) {
        capture = {};
        return pattern == address;
    }
    std::string_view prefix = pattern.substr(0, open);
    std::string_view suffix = pattern.substr(close + 1);
    if (address.size() < prefix.size() + suffix.size()) return false;
    if (address.substr(0, prefix.size()) != prefix) return false;
    if (address.substr(address.size() - suffix.size()) != suffix) return false;
    capture = address.substr(prefix.size(),
                              address.size() - prefix.size() - suffix.size());
    return !capture.empty();
}

// ---------------------------------------------------------------------------
// /entity/layer/{id}/{param} built-in namespace (ADR-0028)

enum class LayerParamKind {
    Opacity, PosX, PosY, ScaleX, ScaleY, Rotation, Remote, Text
};

struct LayerAddress {
    std::string    id;
    LayerParamKind kind;
    const char*    paramString;  // the IPluginContext::setRemoteParam key
};

inline bool isValidLayerIdChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// Returns true if the address belongs to the /entity/layer/ namespace,
// regardless of whether the id or param are valid. Used in dispatch() so
// that malformed layer addresses are dropped with a rate-limited log rather
// than falling through to the mapping-table walk.
inline bool isLayerNamespace(std::string_view address) {
    constexpr std::string_view kPrefix = "/entity/layer/";
    return address.size() > kPrefix.size() &&
           address.compare(0, kPrefix.size(), kPrefix) == 0;
}

inline std::optional<LayerAddress>
parseLayerAddress(std::string_view address) {
    constexpr std::string_view kPrefix = "/entity/layer/";
    if (address.size() <= kPrefix.size() ||
        address.compare(0, kPrefix.size(), kPrefix) != 0) {
        return std::nullopt;
    }
    std::string_view rest = address.substr(kPrefix.size());
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) return std::nullopt;
    std::string_view id = rest.substr(0, slash);
    std::string_view param = rest.substr(slash + 1);
    for (char c : id) if (!isValidLayerIdChar(c)) return std::nullopt;

    struct Entry { std::string_view path; LayerParamKind kind; const char* key; };
    static constexpr Entry kParams[] = {
        {"opacity",  LayerParamKind::Opacity,  "opacity"},
        {"pos/x",    LayerParamKind::PosX,     "pos/x"},
        {"pos/y",    LayerParamKind::PosY,     "pos/y"},
        {"scale/x",  LayerParamKind::ScaleX,   "scale/x"},
        {"scale/y",  LayerParamKind::ScaleY,   "scale/y"},
        {"rotation", LayerParamKind::Rotation, "rotation"},
        {"remote",   LayerParamKind::Remote,   "remote"},
        {"text",     LayerParamKind::Text,     "text"},
    };
    for (const auto& e : kParams) {
        if (param == e.path) {
            return LayerAddress{std::string(id), e.kind, e.key};
        }
    }
    return std::nullopt;
}

// First 's'-typed OSC argument, or nullopt. Iterates all typeTag chars
// after the comma, advancing the arg cursor by wire size (4 bytes for
// i/f/r, 8 for h/d/t) until the first 's' tag is reached; then reads the
// 4-byte-padded null-terminated string payload and returns it.
inline std::optional<std::string>
firstStringArg(std::string_view typeTag,
               const std::uint8_t* argsBegin,
               const std::uint8_t* argsEnd) {
    if (typeTag.size() < 2 || typeTag[0] != ',') return std::nullopt;
    const std::uint8_t* cursor = argsBegin;
    for (std::size_t i = 1; i < typeTag.size(); ++i) {
        const char tag = typeTag[i];
        const auto avail = static_cast<std::size_t>(argsEnd - cursor);
        if (tag == 's') {
            // Padded null-terminated string
            const std::uint8_t* p = cursor;
            while (p < argsEnd && *p != 0) ++p;
            if (p >= argsEnd) return std::nullopt;
            std::string result(reinterpret_cast<const char*>(cursor),
                               static_cast<std::size_t>(p - cursor));
            return result;
        } else if ((tag == 'i' || tag == 'f' || tag == 'r') && avail >= 4) {
            cursor += 4;
        } else if ((tag == 'h' || tag == 'd' || tag == 't') && avail >= 8) {
            cursor += 8;
        } else {
            // Unknown or insufficient data — stop scanning
            break;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// Argument helpers

inline bool firstNumericArgAsInt64(std::string_view typeTag,
                                    const std::uint8_t* argsBegin,
                                    const std::uint8_t* argsEnd,
                                    std::int64_t& out) {
    if (typeTag.size() < 2 || typeTag[0] != ',') return false;
    char tag = typeTag[1];
    auto avail = argsEnd - argsBegin;
    if (tag == 'i' && avail >= 4) { out = readI32BE(argsBegin); return true; }
    if (tag == 'f' && avail >= 4) { out = static_cast<std::int64_t>(readF32BE(argsBegin)); return true; }
    if (tag == 'h' && avail >= 8) { out = readI64BE(argsBegin); return true; }
    if (tag == 'd' && avail >= 8) { out = static_cast<std::int64_t>(readF64BE(argsBegin)); return true; }
    return false;
}

inline bool firstNumericArgAsDouble(std::string_view typeTag,
                                     const std::uint8_t* argsBegin,
                                     const std::uint8_t* argsEnd,
                                     double& out) {
    if (typeTag.size() < 2 || typeTag[0] != ',') return false;
    char tag = typeTag[1];
    auto avail = argsEnd - argsBegin;
    if (tag == 'i' && avail >= 4) { out = static_cast<double>(readI32BE(argsBegin)); return true; }
    if (tag == 'f' && avail >= 4) { out = static_cast<double>(readF32BE(argsBegin)); return true; }
    if (tag == 'h' && avail >= 8) { out = static_cast<double>(readI64BE(argsBegin)); return true; }
    if (tag == 'd' && avail >= 8) { out = readF64BE(argsBegin); return true; }
    return false;
}

inline bool parseDouble(std::string_view s, double& out) {
    if (s.empty()) return false;
    std::string buf(s);
    char* end = nullptr;
    out = std::strtod(buf.c_str(), &end);
    return end != buf.c_str();
}

// ---------------------------------------------------------------------------
// Template expansion

inline std::optional<std::string> expandTemplate(const std::string& tmpl,
                                                   std::string_view capture,
                                                   std::string_view typeTag,
                                                   const std::uint8_t* argsBegin,
                                                   const std::uint8_t* argsEnd) {
    if (tmpl.empty()) return std::string{};

    std::string result;
    result.reserve(tmpl.size() + 16);
    for (std::size_t i = 0; i < tmpl.size(); ) {
        if (tmpl[i] != '$') { result.push_back(tmpl[i++]); continue; }
        std::string_view rest(tmpl.c_str() + i, tmpl.size() - i);
        if (rest.size() >= 6 && rest.substr(0, 6) == "$arg0i" &&
            (rest.size() == 6 || !std::isalnum((unsigned char)rest[6]))) {
            std::int64_t v = 0;
            if (!firstNumericArgAsInt64(typeTag, argsBegin, argsEnd, v)) return std::nullopt;
            char buf[32];
            int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
            if (n > 0) result.append(buf, std::size_t(n));
            i += 6;
        } else if (rest.size() >= 6 && rest.substr(0, 6) == "$arg0f" &&
                   (rest.size() == 6 || !std::isalnum((unsigned char)rest[6]))) {
            double v = 0.0;
            if (!firstNumericArgAsDouble(typeTag, argsBegin, argsEnd, v)) return std::nullopt;
            char buf[32];
            int n = std::snprintf(buf, sizeof(buf), "%.10g", v);
            if (n > 0) result.append(buf, std::size_t(n));
            i += 6;
        } else if (rest.size() >= 9 && rest.substr(0, 9) == "$capturei" &&
                   (rest.size() == 9 || !std::isalnum((unsigned char)rest[9]))) {
            if (capture.empty()) return std::nullopt;
            std::string capStr(capture);
            char* end = nullptr;
            std::int64_t v = std::strtoll(capStr.c_str(), &end, 10);
            if (end == capStr.c_str()) return std::nullopt;
            char buf[32];
            int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
            if (n > 0) result.append(buf, std::size_t(n));
            i += 9;
        } else if (rest.size() >= 9 && rest.substr(0, 9) == "$capturef" &&
                   (rest.size() == 9 || !std::isalnum((unsigned char)rest[9]))) {
            double v = 0.0;
            if (!parseDouble(capture, v)) return std::nullopt;
            char buf[32];
            int n = std::snprintf(buf, sizeof(buf), "%.10g", v);
            if (n > 0) result.append(buf, std::size_t(n));
            i += 9;
        } else {
            result.push_back(tmpl[i++]);
        }
    }
    return result;
}

} // namespace osc_inbound
