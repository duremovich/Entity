// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#if defined(TRACY_ENABLE)
#  include <tracy/Tracy.hpp>
#endif
//
// osc-receiver -- minimal inbound-OSC plugin.
//
// What it does:
//   - Opens a UDP socket bound to 0.0.0.0:<port> (default 53000, override
//     via the ENTITY_OSC_PORT environment variable).
//   - Spawns a worker thread that loops on recvfrom, parses each datagram
//     as an OSC 1.0 message or bundle, and routes recognised addresses to
//     Director commands via IPluginContext::enqueueCommand.
//   - Joins the worker on engine shutdown via a registered shutdown hook
//     so we never use the dispatcher / bus after they've been torn down.
//
// Address namespace (default, overridable via per-project oscInboundMappingsJson):
//
//   /entity/play                  -> Play
//   /entity/pause                 -> Pause
//   /entity/stop                  -> Pause + SeekToFrame{frame:0}
//   /entity/section/next          -> SectionGo
//   /entity/cue/{number}/go       -> FireCue{number}
//   /entity/seek <int frame>      -> SeekToFrame{frame}
//
//   /entity/muncher/up            -> SetInputChannel muncher.input.x = 0, .y = -1
//   /entity/muncher/down          -> SetInputChannel muncher.input.x = 0, .y = +1
//   /entity/muncher/left          -> SetInputChannel muncher.input.x = -1, .y = 0
//   /entity/muncher/right         -> SetInputChannel muncher.input.x = +1, .y = 0
//   /entity/muncher/stop          -> SetInputChannel both axes = 0
//   /entity/muncher/input/x <f>   -> SetInputChannel muncher.input.x = value
//   /entity/muncher/input/y <f>   -> SetInputChannel muncher.input.y = value
//
// The number in `/entity/cue/{number}/go` is parsed as a double so QLab-
// style cue numbers like 1.5 or 2.10 work. `/entity/seek` accepts any
// numeric OSC argument type (i, h, f, d) and truncates to integer frame.
//
// The Muncher UDLR addresses take no arguments — fire on press; the player
// keeps moving in that direction until another direction (or `/stop`) is
// fired. Pattern matches a Bitfocus Companion button setup. The analog
// `/entity/muncher/input/x` / `/y` variants accept any numeric OSC arg
// (i/h/f/d) and clamp to [-1, 1] downstream — for TouchOSC faders, OSC-
// over-Bluetooth controllers, audio-reactive senders, etc.
//
// OSC 1.0 parser is hand-rolled (no oscpack dependency). It handles
// padded strings, the `,typetag` line, integer/float/double args, and
// `#bundle` packets recursively. Anything outside that minimal slice is
// ignored (logged at Debug level).

#include "entity/plugin/PluginContext.hpp"
#include "OscInboundMappings.hpp"

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
  using socklen_t = int;
  static constexpr socket_t ENTITY_OSC_INVALID = INVALID_SOCKET;
  inline int entity_osc_close(socket_t s) { return ::closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_t = int;
  static constexpr socket_t ENTITY_OSC_INVALID = -1;
  inline int entity_osc_close(socket_t s) { return ::close(s); }
#endif

#include <atomic>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using entity::plugin::IPluginContext;
using entity::plugin::LogLevel;

// Shared route-table types and logic from the header.
using osc_inbound::CommandSpec;
using osc_inbound::InboundRoute;
using osc_inbound::defaultRoutes;
using osc_inbound::matchPattern;
using osc_inbound::expandTemplate;

constexpr std::uint16_t kDefaultPort = 53000;

// ---------------------------------------------------------------------------
// Singleton listener state.

struct ReceiverState {
    std::atomic<bool>       running{false};
    socket_t                socket{ENTITY_OSC_INVALID};
    std::thread             worker;
    IPluginContext*         ctx{nullptr};

    // Route table — rebuilt on hash change. Protected by routeMutex so the
    // editor UI thread can write it while the worker reads.
    mutable std::shared_mutex      routeMutex;
    std::vector<InboundRoute>      routeTable;
    std::uint32_t                  lastMappingsHash{0};
};

ReceiverState& state() {
    static ReceiverState s;
    return s;
}

void pluginLog(LogLevel level, std::string_view msg) {
    auto* ctx = state().ctx;
    if (ctx) {
        ctx->log(level, msg);
    }
}

// ---------------------------------------------------------------------------
// OSC 1.0 wire-format helpers — plugin-local (Winsock types involved).

std::uint32_t readU32BE(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) |
           (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8)  |
            std::uint32_t(p[3]);
}

// Read a 4-byte-padded null-terminated OSC string starting at `pos`. On
// success returns true and advances `pos` past the padded run.
bool readOscString(const std::uint8_t* bytes, std::size_t len,
                   std::size_t& pos, std::string_view& out) {
    if (pos >= len) return false;
    std::size_t start = pos;
    while (pos < len && bytes[pos] != 0) ++pos;
    if (pos >= len) return false; // no null terminator
    std::size_t strLen = pos - start;
    out = std::string_view(reinterpret_cast<const char*>(bytes + start), strLen);
    ++pos; // past null
    pos = (pos + 3u) & ~std::size_t(3); // round to 4-byte boundary
    return pos <= len;
}

// ---------------------------------------------------------------------------
// FNV-1a 32-bit hash — used for hot-reload change detection.

std::uint32_t fnv1a32(std::string_view s) {
    std::uint32_t h = 0x811c9dc5u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x01000193u;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Parse the per-project oscInboundMappingsJson. Thin wrapper around the
// shared header parser; adds plugin-level logging on known error paths.

std::vector<InboundRoute> parseInboundMappingsJson(std::string_view json,
                                                    IPluginContext* ctx) {
    if (json.empty()) return defaultRoutes();

    // Fast structural check so we can log a specific warning before
    // delegating — the header parser falls back silently.
    std::size_t p = 0;
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
                                json[p] == '\n' || json[p] == '\r')) ++p;
    if (p >= json.size() || json[p] != '[') {
        if (ctx) ctx->log(LogLevel::Warn,
            "osc-receiver: inbound mappings JSON is not an array — using defaults");
        return defaultRoutes();
    }

    return osc_inbound::parseInboundMappingsJson(json);
}

// ---------------------------------------------------------------------------
// Address routing.

void dispatch(std::string_view address, std::string_view typeTag,
              const std::uint8_t* argsBegin, const std::uint8_t* argsEnd) {
    auto* ctx = state().ctx;
    if (!ctx) return;

    auto& s = state();
    std::shared_lock<std::shared_mutex> lock(s.routeMutex);

    for (const auto& route : s.routeTable) {
        std::string_view capture;
        if (!matchPattern(route.addressPattern, address, capture)) continue;

        for (const auto& cmd : route.commands) {
            if (cmd.commandType.empty()) continue;
            bool hasTokens = (cmd.paramsTemplate.find('$') != std::string::npos);
            if (!hasTokens) {
                ctx->enqueueCommand(cmd.commandType, cmd.paramsTemplate);
            } else {
                auto params = expandTemplate(cmd.paramsTemplate,
                                             capture, typeTag,
                                             argsBegin, argsEnd);
                if (!params) {
                    std::string m = "osc-receiver: ";
                    m.append(address);
                    m.append(" dropped — missing or non-numeric argument");
                    pluginLog(LogLevel::Warn, m);
                    continue;
                }
                ctx->enqueueCommand(cmd.commandType, *params);
            }
        }
        // First matching route wins.
        return;
    }

    std::string m = "ignored OSC address: ";
    m.append(address);
    pluginLog(LogLevel::Debug, m);
}

// Parse one OSC packet. Recurses into bundle elements.
void parsePacket(const std::uint8_t* bytes, std::size_t len) {
    if (len == 0) return;
    if (len >= 8 && std::memcmp(bytes, "#bundle\0", 8) == 0) {
        if (len < 16) return;
        std::size_t pos = 16;
        while (pos + 4 <= len) {
            std::uint32_t elemLen = readU32BE(bytes + pos);
            pos += 4;
            if (elemLen == 0 || pos + elemLen > len) return;
            parsePacket(bytes + pos, elemLen);
            pos += elemLen;
        }
        return;
    }

    std::size_t pos = 0;
    std::string_view address;
    if (!readOscString(bytes, len, pos, address)) return;
    std::string_view typeTag;
    if (!readOscString(bytes, len, pos, typeTag)) {
        typeTag = std::string_view();
    }
    dispatch(address, typeTag, bytes + pos, bytes + len);
}

// ---------------------------------------------------------------------------
// UDP listener thread.

void workerLoop() {
#if defined(TRACY_ENABLE)
    tracy::SetThreadName("OSC");
#endif
    constexpr std::size_t kBufSize = 4096;
    std::vector<std::uint8_t> buf(kBufSize);
    auto& s = state();
    while (s.running.load(std::memory_order_acquire)) {
        if (s.ctx) {
            std::string mappingsJson =
                s.ctx->getStringSetting("oscInboundMappingsJson", "");
            const std::uint32_t h = fnv1a32(mappingsJson);
            if (h != s.lastMappingsHash) {
                s.lastMappingsHash = h;
                std::vector<InboundRoute> newTable =
                    mappingsJson.empty()
                        ? defaultRoutes()
                        : parseInboundMappingsJson(mappingsJson, s.ctx);
                std::unique_lock<std::shared_mutex> wlock(s.routeMutex);
                s.routeTable = std::move(newTable);
            }
        }

        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int n = ::recvfrom(s.socket,
                           reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()),
                           0,
                           reinterpret_cast<sockaddr*>(&from),
                           &fromLen);
        if (n > 0) {
            parsePacket(buf.data(), static_cast<std::size_t>(n));
            continue;
        }
#ifdef _WIN32
        int err = ::WSAGetLastError();
        if (err == WSAETIMEDOUT) continue;
        if (err == WSAEINTR) continue;
        if (!s.running.load(std::memory_order_acquire)) break;
#else
        if (!s.running.load(std::memory_order_acquire)) break;
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool startListener(std::uint16_t port) {
    auto& s = state();

#ifdef _WIN32
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        pluginLog(LogLevel::Error, "WSAStartup failed");
        return false;
    }
#endif

    s.socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s.socket == ENTITY_OSC_INVALID) {
        pluginLog(LogLevel::Error, "socket() failed for UDP listener");
#ifdef _WIN32
        ::WSACleanup();
#endif
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (::bind(s.socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        char m[128];
        std::snprintf(m, sizeof(m), "bind() failed on UDP port %u", unsigned(port));
        pluginLog(LogLevel::Error, m);
        entity_osc_close(s.socket);
        s.socket = ENTITY_OSC_INVALID;
#ifdef _WIN32
        ::WSACleanup();
#endif
        return false;
    }

#ifdef _WIN32
    DWORD timeoutMs = 250;
    ::setsockopt(s.socket, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    timeval tv{};
    tv.tv_sec  = 0;
    tv.tv_usec = 250'000;
    ::setsockopt(s.socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    s.running.store(true, std::memory_order_release);
    s.worker = std::thread(&workerLoop);

    char m[128];
    std::snprintf(m, sizeof(m),
                  "osc-receiver listening on UDP 0.0.0.0:%u", unsigned(port));
    pluginLog(LogLevel::Info, m);
    return true;
}

extern "C" void entity_plugin_osc_receiver_shutdown() {
    auto& s = state();
    if (!s.running.exchange(false)) return;
    if (s.socket != ENTITY_OSC_INVALID) {
        entity_osc_close(s.socket);
        s.socket = ENTITY_OSC_INVALID;
    }
    if (s.worker.joinable()) {
        s.worker.join();
    }
#ifdef _WIN32
    ::WSACleanup();
#endif
    if (s.ctx) {
        s.ctx->log(LogLevel::Info, "osc-receiver stopped");
    }
    s.ctx = nullptr;
}

std::uint16_t resolvePort(IPluginContext* ctx) {
    if (const char* env = std::getenv("ENTITY_OSC_PORT")) {
        int v = 0;
        auto [ptr, ec] = std::from_chars(env, env + std::strlen(env), v);
        if (ec == std::errc() && v > 0 && v < 65536) {
            return static_cast<std::uint16_t>(v);
        }
    }
    int port = ctx->getIntSetting("oscReceiverPort", kDefaultPort);
    if (port < 1 || port > 65535) port = kDefaultPort;
    return static_cast<std::uint16_t>(port);
}

} // namespace

extern "C" int entity_plugin_register_osc_receiver(IPluginContext* ctx) {
    if (ctx == nullptr) return -1;
    if (ctx->apiVersion() != entity::plugin::PLUGIN_API_VERSION) {
        ctx->log(LogLevel::Error,
                 std::string("API version mismatch: engine=") +
                 std::to_string(ctx->apiVersion()) +
                 " plugin=" +
                 std::to_string(entity::plugin::PLUGIN_API_VERSION));
        return -2;
    }

    state().ctx = ctx;

    if (!ctx->getBoolSetting("oscReceiverEnabled", true)) {
        ctx->log(LogLevel::Info,
                 "osc-receiver disabled in Preferences — no UDP listener");
        state().ctx = nullptr;
        return 0;
    }

    {
        std::string mappingsJson = ctx->getStringSetting("oscInboundMappingsJson", "");
        auto& s = state();
        s.lastMappingsHash = fnv1a32(mappingsJson);
        s.routeTable = mappingsJson.empty()
            ? defaultRoutes()
            : parseInboundMappingsJson(mappingsJson, ctx);
    }

    if (!startListener(resolvePort(ctx))) {
        state().ctx = nullptr;
        return -3;
    }

    ctx->registerShutdownHook(&entity_plugin_osc_receiver_shutdown);
    return 0;
}
