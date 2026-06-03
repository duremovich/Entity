// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#if defined(TRACY_ENABLE)
#  include <tracy/Tracy.hpp>
#endif
//
// osc-sender -- outbound OSC over UDP.
//
// What it does:
//   - Opens a UDP send socket (SOCK_DGRAM, no bind).
//   - Spawns a 30 Hz worker thread that polls getTransportSnapshot(), diffs
//     against last-sent state, and broadcasts OSC packets to all enabled
//     destinations from Settings "oscSenderDestinationsJson".
//   - Multiple events per tick are coalesced into one OSC bundle.
//   - Joins the worker on engine shutdown via a registered shutdown hook.
//
// Default broadcast namespace:
//
//   /entity/out/transport/state    s  "stopped"|"playing"|"paused"   on-change
//   /entity/out/transport/frame    i  <frameNumber>                   30 Hz when playing
//   /entity/out/transport/seconds  f  <seconds>                       30 Hz when playing
//   /entity/out/section/active/index  i  <activeSectionIndex>         on-change
//   /entity/out/section/active/frame  i  <activeSectionFrame>         on-change
//   /entity/out/section/next/index    i  <nextSectionIndex>           on-change
//   /entity/out/section/next/frame    i  <nextSectionFrame>           on-change
//   /entity/out/project/name          s  <projectName>                on-change
//   /entity/out/heartbeat             i  <counter>                    1 Hz (every ~30 ticks)
//
// Destinations JSON shape (Settings "oscSenderDestinationsJson"):
//   [{"host":"192.168.1.50","port":8000,"enabled":true}, ...]

#include "entity/plugin/PluginContext.hpp"
#include "OscWire.hpp"

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t  = SOCKET;
  static constexpr socket_t ENTITY_OSC_SENDER_INVALID = INVALID_SOCKET;
  inline int entity_osc_sender_close(socket_t s) { return ::closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_t = int;
  static constexpr socket_t ENTITY_OSC_SENDER_INVALID = -1;
  inline int entity_osc_sender_close(socket_t s) { return ::close(s); }
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

using entity::plugin::IPluginContext;
using entity::plugin::LogLevel;
using entity::plugin::SignalArgPod;
using entity::plugin::SignalEmitPod;
using entity::plugin::TransportSnapshot;
using entity::plugin::TransportState;

// ---------------------------------------------------------------------------
// Outbound mapping override types — defined before SenderState which holds one.

struct OutboundOverride {
    bool        enabled{true};
    std::string address;  // empty = use the default address for this event
};

using OverrideMap = std::unordered_map<std::string, OutboundOverride>;

// ---------------------------------------------------------------------------
// Singleton sender state.

struct SenderState {
    std::atomic<bool>   running{false};
    socket_t            socket{ENTITY_OSC_SENDER_INVALID};
    std::thread         worker;
    IPluginContext*     ctx{nullptr};

    std::mutex              wakeMutex;
    std::condition_variable wakeCv;

    // Resolved destination list — populated once at register time, read-only
    // from the worker thread. No mutex needed (written before worker starts).
    std::vector<sockaddr_in> destinations;

    // Outbound mapping overrides — polled each tick via hash comparison.
    // Written and read only by the worker thread; no mutex needed.
    std::uint32_t lastMappingsHash{0};   // FNV-1a hash of last-seen JSON
    OverrideMap   mappingOverrides;       // rebuilt on hash change

    // sendto failure rate-limiting: log at most once per second.
    int           sendFailsSinceLastLog{0};
    int           sendFailTicksSinceLog{0}; // ticks since last failure-log emit

    // Inbound signal emit queue. Filled by the host via drainSignalEmits()
    // at the top of each worker tick. Capacity bounded in the host; no
    // secondary cap here — the host's 256-deep limit is authoritative.
    static constexpr std::size_t kDrainBatch = 64;
    SignalEmitPod signalBatch[kDrainBatch]{};
};

SenderState& state() {
    static SenderState s;
    return s;
}

void pluginLog(LogLevel level, const char* msg) {
    auto* ctx = state().ctx;
    if (ctx) ctx->log(level, msg);
}

// ---------------------------------------------------------------------------
// FNV-1a 32-bit hash — fast, no GPL pull-in.
// Event id strings for the override map:
//   "transport.state"      "transport.frame"    "transport.seconds"
//   "section.active.index" "section.active.frame"
//   "section.next.index"   "section.next.frame"
//   "project.name"         "heartbeat"

// FNV-1a 32-bit — fast, header-only, no GPL pull-in.
std::uint32_t fnv1a32(std::string_view s) {
    std::uint32_t h = 0x811c9dc5u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x01000193u;
    }
    return h;
}

// JSON whitespace helper used by both hand-rolled parsers below.
inline bool isJsonWs(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Parse "oscOutboundMappingsJson" into an OverrideMap. Hand-rolled for the
// same Apache-2.0 boundary reason as parseDestinationsJson.
//
// Expected shape:
//   { "events": { "transport.frame": { "enabled": true, "address": "/foo" }, ... } }
//
// Outer object is located; "events" value is extracted as a substring;
// each key:object pair inside "events" is parsed for "enabled" and "address".
// Malformed JSON returns an empty map with a single log line.
OverrideMap parseOutboundMappingsJson(const std::string& json, IPluginContext* ctx) {
    OverrideMap out;
    if (json.empty()) return out;

    // Find the "events" value object substring.
    const std::string eventsKey = "\"events\"";
    auto evPos = json.find(eventsKey);
    if (evPos == std::string::npos) return out; // empty events = all defaults

    evPos += eventsKey.size();
    while (evPos < json.size() && (isJsonWs(json[evPos]) || json[evPos] == ':')) ++evPos;
    if (evPos >= json.size() || json[evPos] != '{') {
        if (ctx) ctx->log(LogLevel::Warn,
                          "osc-sender: oscOutboundMappingsJson 'events' value is not an object");
        return out;
    }

    // Find the matching closing brace for the events object.
    // We need to handle one level of nesting (event-id -> {...}).
    std::size_t depth = 0;
    std::size_t evStart = evPos;
    std::size_t evEnd = std::string::npos;
    for (std::size_t i = evPos; i < json.size(); ++i) {
        if (json[i] == '{') ++depth;
        else if (json[i] == '}') {
            --depth;
            if (depth == 0) { evEnd = i; break; }
        }
    }
    if (evEnd == std::string::npos) {
        if (ctx) ctx->log(LogLevel::Warn,
                          "osc-sender: oscOutboundMappingsJson 'events' object is unterminated");
        return out;
    }

    // Walk the events object looking for "eventId": { ... } pairs.
    std::string_view evObj(json.data() + evStart + 1, evEnd - evStart - 1);
    std::size_t pos = 0;
    while (pos < evObj.size()) {
        // Find next quoted key.
        auto q1 = evObj.find('"', pos);
        if (q1 == std::string_view::npos) break;
        auto q2 = evObj.find('"', q1 + 1);
        if (q2 == std::string_view::npos) break;
        std::string eventId(evObj.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;

        // Skip to the inner object.
        while (pos < evObj.size() && (isJsonWs(evObj[pos]) || evObj[pos] == ':')) ++pos;
        if (pos >= evObj.size() || evObj[pos] != '{') break;
        auto innerStart = pos;
        auto innerEnd = evObj.find('}', innerStart);
        if (innerEnd == std::string_view::npos) break;
        std::string_view inner = evObj.substr(innerStart, innerEnd - innerStart + 1);
        pos = innerEnd + 1;

        // Parse "enabled" and "address" from the inner object.
        OutboundOverride ov;

        auto findBool = [&](std::string_view key) -> int {
            std::string needle = "\""; needle += key; needle += "\"";
            auto p = inner.find(needle);
            if (p == std::string_view::npos) return -1;
            p += needle.size();
            while (p < inner.size() && (isJsonWs(inner[p]) || inner[p] == ':')) ++p;
            if (p + 4 <= inner.size() && inner.substr(p, 4) == "true")  return 1;
            if (p + 5 <= inner.size() && inner.substr(p, 5) == "false") return 0;
            return -1;
        };

        auto findStr = [&](std::string_view key) -> std::string {
            std::string needle = "\""; needle += key; needle += "\"";
            auto p = inner.find(needle);
            if (p == std::string_view::npos) return {};
            p += needle.size();
            while (p < inner.size() && (isJsonWs(inner[p]) || inner[p] == ':')) ++p;
            if (p >= inner.size() || inner[p] != '"') return {};
            ++p;
            std::string val;
            while (p < inner.size() && inner[p] != '"') {
                if (inner[p] == '\\' && p + 1 < inner.size()) ++p;
                val += inner[p++];
            }
            return val;
        };

        const int en = findBool("enabled");
        if (en == 0) ov.enabled = false;
        ov.address = findStr("address");

        out[eventId] = std::move(ov);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Destination JSON parser.
//
// Hand-rolled to keep the plugin free of non-plugin-api C++ dependencies,
// consistent with OscReceiverPlugin.cpp which also hand-rolls its parsing.
// Parses only the specific shape used here:
//   [{"host":"...","port":N,"enabled":true/false}, ...]
//
// Strategy: walk the string with simple string searches — no recursive
// descent needed for this fixed schema. Each object is located by finding
// the { } span, then individual fields within it. Invalid entries are
// skipped with a log; malformed outer JSON returns an empty list.

std::vector<sockaddr_in> parseDestinationsJson(const std::string& json,
                                                IPluginContext* ctx) {
    std::vector<sockaddr_in> out;
    if (json.empty()) return out;

    // Helper: find the value string for a JSON key within a substring.
    // Returns empty string_view if not found.
    auto findStringValue = [](std::string_view obj, std::string_view key) -> std::string {
        // Look for "key":"value"
        std::string needle = "\"";
        needle += key; needle += "\"";
        auto pos = obj.find(needle);
        if (pos == std::string_view::npos) return {};
        pos += needle.size();
        while (pos < obj.size() && (isJsonWs(obj[pos]) || obj[pos] == ':')) ++pos;
        if (pos >= obj.size() || obj[pos] != '"') return {};
        ++pos; // skip opening quote
        std::string val;
        while (pos < obj.size() && obj[pos] != '"') {
            if (obj[pos] == '\\' && pos + 1 < obj.size()) { ++pos; } // skip escape
            val += obj[pos++];
        }
        return val;
    };

    auto findNumberValue = [](std::string_view obj, std::string_view key) -> long {
        std::string needle = "\"";
        needle += key; needle += "\"";
        auto pos = obj.find(needle);
        if (pos == std::string_view::npos) return -1;
        pos += needle.size();
        while (pos < obj.size() && (isJsonWs(obj[pos]) || obj[pos] == ':')) ++pos;
        if (pos >= obj.size()) return -1;
        char* end = nullptr;
        long v = std::strtol(obj.data() + pos, &end, 10);
        return (end != obj.data() + pos) ? v : -1;
    };

    auto findBoolValue = [](std::string_view obj, std::string_view key) -> int {
        std::string needle = "\"";
        needle += key; needle += "\"";
        auto pos = obj.find(needle);
        if (pos == std::string_view::npos) return -1;
        pos += needle.size();
        while (pos < obj.size() && (isJsonWs(obj[pos]) || obj[pos] == ':')) ++pos;
        if (pos + 4 <= obj.size() && obj.substr(pos, 4) == "true")  return 1;
        if (pos + 5 <= obj.size() && obj.substr(pos, 5) == "false") return 0;
        return -1;
    };

    std::string_view sv(json);
    std::size_t searchPos = 0;
    int entryIdx = 0;
    while (searchPos < sv.size()) {
        auto objStart = sv.find('{', searchPos);
        if (objStart == std::string_view::npos) break;
        // Find matching closing brace (no nesting in our schema).
        auto objEnd = sv.find('}', objStart);
        if (objEnd == std::string_view::npos) break;
        std::string_view obj = sv.substr(objStart, objEnd - objStart + 1);
        searchPos = objEnd + 1;
        ++entryIdx;

        const std::string host  = findStringValue(obj, "host");
        const long        port  = findNumberValue(obj, "port");
        const int         en    = findBoolValue(obj, "enabled");

        if (en == 0) continue; // explicitly disabled

        if (host.empty()) {
            char m[64];
            std::snprintf(m, sizeof(m),
                          "osc-sender: destination %d missing/empty 'host', skipping",
                          entryIdx);
            if (ctx) ctx->log(LogLevel::Warn, m);
            continue;
        }
        if (port < 1 || port > 65535) {
            char m[80];
            std::snprintf(m, sizeof(m),
                          "osc-sender: destination %d port %ld out of range, skipping",
                          entryIdx, port);
            if (ctx) ctx->log(LogLevel::Warn, m);
            continue;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<std::uint16_t>(port));
#ifdef _WIN32
        if (::InetPtonA(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
#else
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
#endif
            char m[128];
            std::snprintf(m, sizeof(m),
                          "osc-sender: destination %d host '%.*s' is not a valid IPv4 address, skipping",
                          entryIdx, static_cast<int>(host.size()), host.c_str());
            if (ctx) ctx->log(LogLevel::Warn, m);
            continue;
        }

        out.push_back(addr);
    }
    return out;
}

// Wire-format helpers live in OscWire.hpp (shared with unit tests).
// Pull them into the local anon namespace for use without the osc:: prefix.
using osc::writeU32BE;
using osc::writeI32BE;
using osc::writeF32BE;
using osc::writeOscString;
using osc::buildOscMessageInt;
using osc::buildOscMessageFloat;
using osc::buildOscMessageString;
using osc::writeBundleHeader;
using osc::appendBundleMessage;

// ---------------------------------------------------------------------------
// sendto helper — fire a buffer to one destination.
//
// Failure handling: on Windows, sendto to a closed remote port reflects an
// ICMP Port Unreachable back, causing the *next* sendto on the same socket
// to fail with WSAECONNRESET. At 30 Hz that would spam the log. We rate-limit
// to one log line per second (≈30 ticks) using counters on SenderState.

void sendPacket(SenderState& s, const sockaddr_in& dest,
                const std::vector<std::uint8_t>& buf) {
    if (buf.empty()) return;
    const int n = ::sendto(s.socket,
                           reinterpret_cast<const char*>(buf.data()),
                           static_cast<int>(buf.size()),
                           0,
                           reinterpret_cast<const sockaddr*>(&dest),
                           sizeof(dest));
    if (n < 0) {
#ifdef _WIN32
        const int err = ::WSAGetLastError();
#else
        const int err = errno;
#endif
        const bool firstFailure = (s.sendFailsSinceLastLog == 0);
        ++s.sendFailsSinceLastLog;
        // Log immediately on the first failure of a fresh batch, then suppress
        // until the 1s boundary (handled in broadcastTick's heartbeat cadence).
        if (firstFailure && s.ctx) {
            char m[128];
            std::snprintf(m, sizeof(m),
                          "osc-sender: sendto failed (error=%d, count since last log=1)",
                          err);
            s.ctx->log(LogLevel::Warn, m);
        }
    }
}

// ---------------------------------------------------------------------------
// Diff cache — tracks what was last sent so we only re-send on change.

struct LastSent {
    bool        firstTick{true};

    TransportState  state{TransportState::Stopped};

    int             activeSectionIndex{-2}; // -2 = never sent
    std::int64_t    activeSectionFrame{-1};
    int             nextSectionIndex{-2};
    std::int64_t    nextSectionFrame{-1};

    std::string     projectName{"__NEVER_SENT__"};

    std::uint32_t   heartbeatCounter{0};
    int             ticksSinceHeartbeat{0};
};

// ---------------------------------------------------------------------------
// Build the diagnostic event packet for one 30 Hz tick.
//
// Diffs snap against last, updates last exactly once, and returns the
// complete OSC packet bytes (bundle-or-bare). Returns an empty vector if
// nothing needs sending this tick. Callers fan the returned bytes out to
// all destinations via sendPacket — this keeps the diff advancing once per
// tick regardless of destination count.

// Resolve the OSC address for an event, applying any mapping override.
// Returns empty string_view if the event is disabled (suppress emit).
// `defaultAddr` must be a compile-time literal — it's used as fallback.
std::string_view resolveAddress(const OverrideMap& overrides,
                                const char* eventId,
                                const char* defaultAddr) {
    auto it = overrides.find(eventId);
    if (it == overrides.end()) return defaultAddr;
    if (!it->second.enabled) return {};
    return it->second.address.empty() ? std::string_view(defaultAddr)
                                      : std::string_view(it->second.address);
}

std::vector<std::uint8_t> buildTickPacket(const TransportSnapshot& snap,
                                           LastSent& last,
                                           SenderState& s) {
    const OverrideMap& ov = s.mappingOverrides;

    // Messages accumulated this tick. We send a single bundle when >1 fires,
    // or a bare message when exactly 1 fires (simpler for receivers with
    // minimal bundle support). Either way the receiver sees valid OSC.
    std::vector<std::vector<std::uint8_t>> messages;
    messages.reserve(9);

    std::vector<std::uint8_t> msg;

    // -- transport.state (on-change) ----------------------------------------
    if (last.firstTick || snap.playbackState != last.state) {
        last.state = snap.playbackState;
        auto addr = resolveAddress(ov, "transport.state",
                                   "/entity/out/transport/state");
        if (!addr.empty()) {
            std::string_view sv =
                snap.playbackState == TransportState::Playing  ? "playing"  :
                snap.playbackState == TransportState::Paused   ? "paused"   :
                                                                 "stopped";
            buildOscMessageString(msg, addr, sv);
            messages.push_back(msg);
        }
    }

    // -- transport.frame + transport.seconds (30 Hz when playing) -----------
    if (snap.playbackState == TransportState::Playing) {
        auto addrFrame = resolveAddress(ov, "transport.frame",
                                        "/entity/out/transport/frame");
        if (!addrFrame.empty()) {
            buildOscMessageInt(msg, addrFrame,
                               static_cast<std::int32_t>(snap.frameNumber));
            messages.push_back(msg);
        }

        auto addrSec = resolveAddress(ov, "transport.seconds",
                                      "/entity/out/transport/seconds");
        if (!addrSec.empty()) {
            const float seconds = snap.frameRate > 0.0
                ? static_cast<float>(snap.frameNumber / snap.frameRate)
                : 0.0f;
            buildOscMessageFloat(msg, addrSec, seconds);
            messages.push_back(msg);
        }
    }

    // -- section.active (on-change) -----------------------------------------
    if (last.firstTick || snap.activeSectionIndex != last.activeSectionIndex) {
        last.activeSectionIndex = snap.activeSectionIndex;
        auto addr = resolveAddress(ov, "section.active.index",
                                   "/entity/out/section/active/index");
        if (!addr.empty()) {
            buildOscMessageInt(msg, addr, snap.activeSectionIndex);
            messages.push_back(msg);
        }
    }
    if (last.firstTick || snap.activeSectionFrame != last.activeSectionFrame) {
        last.activeSectionFrame = snap.activeSectionFrame;
        auto addr = resolveAddress(ov, "section.active.frame",
                                   "/entity/out/section/active/frame");
        if (!addr.empty()) {
            buildOscMessageInt(msg, addr,
                               static_cast<std::int32_t>(snap.activeSectionFrame));
            messages.push_back(msg);
        }
    }

    // -- section.next (on-change) -------------------------------------------
    if (last.firstTick || snap.nextSectionIndex != last.nextSectionIndex) {
        last.nextSectionIndex = snap.nextSectionIndex;
        auto addr = resolveAddress(ov, "section.next.index",
                                   "/entity/out/section/next/index");
        if (!addr.empty()) {
            buildOscMessageInt(msg, addr, snap.nextSectionIndex);
            messages.push_back(msg);
        }
    }
    if (last.firstTick || snap.nextSectionFrame != last.nextSectionFrame) {
        last.nextSectionFrame = snap.nextSectionFrame;
        auto addr = resolveAddress(ov, "section.next.frame",
                                   "/entity/out/section/next/frame");
        if (!addr.empty()) {
            buildOscMessageInt(msg, addr,
                               static_cast<std::int32_t>(snap.nextSectionFrame));
            messages.push_back(msg);
        }
    }

    // -- project.name (on-change) -------------------------------------------
    if (last.firstTick || snap.projectName != last.projectName) {
        last.projectName = snap.projectName;
        auto addr = resolveAddress(ov, "project.name",
                                   "/entity/out/project/name");
        if (!addr.empty()) {
            buildOscMessageString(msg, addr, snap.projectName);
            messages.push_back(msg);
        }
    }

    // -- heartbeat (1 Hz = every ~30 ticks) ---------------------------------
    ++last.ticksSinceHeartbeat;
    if (last.ticksSinceHeartbeat >= 30) {
        last.ticksSinceHeartbeat = 0;
        auto addr = resolveAddress(ov, "heartbeat", "/entity/out/heartbeat");
        if (!addr.empty()) {
            buildOscMessageInt(msg, addr,
                               static_cast<std::int32_t>(last.heartbeatCounter++));
            messages.push_back(msg);
        } else {
            ++last.heartbeatCounter; // keep counter advancing even when suppressed
        }

        // Emit a deferred sendto-failure summary at the 1s boundary. Covers
        // the ticks after the first-failure log where failures were suppressed.
        if (s.sendFailsSinceLastLog > 1 && s.ctx) {
            char m2[128];
            std::snprintf(m2, sizeof(m2),
                          "osc-sender: sendto failed (suppressed %d failures since last log)",
                          s.sendFailsSinceLastLog - 1);
            s.ctx->log(LogLevel::Warn, m2);
        }
        s.sendFailsSinceLastLog = 0;
    }

    last.firstTick = false;

    if (messages.empty()) return {};

    if (messages.size() == 1) {
        return messages[0];
    }

    // Multiple messages — coalesce into one bundle.
    std::vector<std::uint8_t> bundle;
    bundle.reserve(512);
    writeBundleHeader(bundle);
    for (const auto& m : messages) appendBundleMessage(bundle, m);
    return bundle;
}

// ---------------------------------------------------------------------------
// Build a single OSC message from a SignalEmitPod. Encodes the typetag
// string from arg types then appends each arg value. Handles any mix of
// Int, Float, and String args in order.
//
// OSC wire layout for a multi-arg message:
//   [address\0 padded] [typetags\0 padded] [arg0] [arg1] ...
// Single-arg messages use the existing buildOscMessage* helpers (which
// produce identical bytes but are more readable for the single-arg case).

void buildOscSignalMessage(std::vector<std::uint8_t>& out,
                            const SignalEmitPod& emit) {
    out.clear();
    if (emit.argCount == 0) {
        // No-arg bang: address + bare typetag ",".
        writeOscString(out, emit.address);
        writeOscString(out, ",");
        return;
    }
    if (emit.argCount == 1) {
        // Use the single-arg builders for the common case.
        const auto& a = emit.args[0];
        switch (a.type) {
            case SignalArgPod::Type::Int:
                buildOscMessageInt(out, emit.address, a.i);
                return;
            case SignalArgPod::Type::Float:
                buildOscMessageFloat(out, emit.address, a.f);
                return;
            case SignalArgPod::Type::String:
                buildOscMessageString(out, emit.address, a.s);
                return;
        }
        return;
    }

    // Multi-arg: build typetag string then serialize each arg.
    writeOscString(out, emit.address);

    // Typetag string: "," followed by one char per arg.
    std::string typetags(",");
    const std::size_t count = emit.argCount < SignalEmitPod::kMaxArgs
                                ? emit.argCount : SignalEmitPod::kMaxArgs;
    for (std::size_t i = 0; i < count; ++i) {
        switch (emit.args[i].type) {
            case SignalArgPod::Type::Int:    typetags += 'i'; break;
            case SignalArgPod::Type::Float:  typetags += 'f'; break;
            case SignalArgPod::Type::String: typetags += 's'; break;
        }
    }
    writeOscString(out, typetags);

    // Arg values in order.
    for (std::size_t i = 0; i < count; ++i) {
        const auto& a = emit.args[i];
        switch (a.type) {
            case SignalArgPod::Type::Int:    writeI32BE(out, a.i);                break;
            case SignalArgPod::Type::Float:  writeF32BE(out, a.f);                break;
            case SignalArgPod::Type::String: writeOscString(out, a.s);            break;
        }
    }
}

// Drain the host's pending signal emits and transmit each to all destinations.
// Called at the top of each worker tick. Signal emits are polled every ~33ms
// (the worker's natural sleep interval); the producer path does not notify the
// cv, so up to one tick of latency is expected and accepted for Phase 1.
void drainAndSendSignals(SenderState& s) {
    if (!s.ctx || s.destinations.empty()) return;

    const std::size_t n = s.ctx->drainSignalEmits(s.signalBatch,
                                                   SenderState::kDrainBatch);
    if (n == 0) return;

    std::vector<std::uint8_t> msg;
    for (std::size_t i = 0; i < n; ++i) {
        buildOscSignalMessage(msg, s.signalBatch[i]);
        if (!msg.empty()) {
            for (const auto& dest : s.destinations) {
                sendPacket(s, dest, msg);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Worker thread — 30 Hz poll loop.

void workerLoop() {
#if defined(TRACY_ENABLE)
    tracy::SetThreadName("OSC-Sender");
#endif
    auto& s = state();

    LastSent last;

    while (s.running.load(std::memory_order_acquire)) {
        if (s.ctx) {
            // Drain and send any pending SignalEmit events first. The poll
            // runs every ~33ms tick; the host producer does not notify the cv
            // so latency is bounded by one tick interval (accepted for v1).
            drainAndSendSignals(s);

            // Hot-reload outbound mapping overrides — poll each tick,
            // rebuild only when the JSON changes (FNV-1a hash gate).
            std::string mappingsJson =
                s.ctx->getStringSetting("oscOutboundMappingsJson", "");
            const std::uint32_t h = fnv1a32(mappingsJson);
            if (h != s.lastMappingsHash) {
                s.lastMappingsHash  = h;
                s.mappingOverrides  = parseOutboundMappingsJson(mappingsJson, s.ctx);
            }

            if (!s.destinations.empty()) {
                const TransportSnapshot snap = s.ctx->getTransportSnapshot();
                const auto packet = buildTickPacket(snap, last, s);
                if (!packet.empty()) {
                    for (const auto& dest : s.destinations) {
                        sendPacket(s, dest, packet);
                    }
                }
            }
        }

        std::unique_lock<std::mutex> lk(s.wakeMutex);
        s.wakeCv.wait_for(lk, std::chrono::milliseconds(33),
                          [&]{ return !s.running.load(std::memory_order_acquire); });
    }
}

// ---------------------------------------------------------------------------
// Socket setup + worker launch.

bool startSender() {
    auto& s = state();

#ifdef _WIN32
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        pluginLog(LogLevel::Error, "osc-sender: WSAStartup failed");
        return false;
    }
#endif

    s.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s.socket == ENTITY_OSC_SENDER_INVALID) {
        pluginLog(LogLevel::Error, "osc-sender: socket() failed for UDP sender");
#ifdef _WIN32
        ::WSACleanup();
#endif
        return false;
    }

    // No bind — outbound datagrams only; the OS assigns an ephemeral source port.

    s.running.store(true, std::memory_order_release);
    s.worker = std::thread(&workerLoop);

    pluginLog(LogLevel::Info, "osc-sender: worker started");
    return true;
}

} // namespace

// Engine-driven shutdown hook. Mirrors osc-receiver teardown ordering:
// signal → wake worker → close socket → join → WSACleanup.
extern "C" void entity_plugin_osc_sender_shutdown() {
    auto& s = state();
    if (!s.running.exchange(false)) return;

    s.wakeCv.notify_all();

    if (s.socket != ENTITY_OSC_SENDER_INVALID) {
        entity_osc_sender_close(s.socket);
        s.socket = ENTITY_OSC_SENDER_INVALID;
    }

    if (s.worker.joinable()) {
        s.worker.join();
    }

#ifdef _WIN32
    ::WSACleanup();
#endif

    if (s.ctx) {
        s.ctx->log(entity::plugin::LogLevel::Info, "osc-sender: stopped");
    }
    s.ctx = nullptr;
}

extern "C" int entity_plugin_register_osc_sender(IPluginContext* ctx) {
    if (ctx == nullptr) return -1;
    if (ctx->apiVersion() != entity::plugin::PLUGIN_API_VERSION) {
        ctx->log(LogLevel::Error,
                 std::string("osc-sender: API version mismatch: engine=") +
                 std::to_string(ctx->apiVersion()) +
                 " plugin=" +
                 std::to_string(entity::plugin::PLUGIN_API_VERSION));
        return -2;
    }

    state().ctx = ctx;

    bool enabled = ctx->getBoolSetting("oscSenderEnabled", false);
    if (!enabled) {
        if (const char* env = std::getenv("ENTITY_OSC_SENDER_ENABLED"))
            enabled = (env[0] == '1');
    }
    if (!enabled) {
        ctx->log(LogLevel::Info,
                 "osc-sender: disabled in Settings — no UDP sender thread");
        state().ctx = nullptr;
        return 0;
    }

    state().destinations = parseDestinationsJson(
        ctx->getStringSetting("oscSenderDestinationsJson", ""), ctx);

    if (state().destinations.empty()) {
        ctx->log(LogLevel::Warn,
                 "osc-sender: enabled but no valid destinations — worker will idle");
    }

    if (!startSender()) {
        state().ctx = nullptr;
        return -3;
    }

    ctx->registerShutdownHook(&entity_plugin_osc_sender_shutdown);
    return 0;
}
