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
// Address namespace (fixed, small):
//
//   /entity/play                  -> Play
//   /entity/pause                 -> Pause
//   /entity/stop                  -> Pause + SeekToFrame{frame:0}
//   /entity/section/next          -> SectionGo
//   /entity/cue/{number}/go       -> FireCue{number}
//   /entity/seek <int frame>      -> SeekToFrame{frame}
//
// The number in `/entity/cue/{number}/go` is parsed as a double so QLab-
// style cue numbers like 1.5 or 2.10 work. `/entity/seek` accepts any
// numeric OSC argument type (i, h, f, d) and truncates to integer frame.
//
// OSC 1.0 parser is hand-rolled (no oscpack dependency). It handles
// padded strings, the `,typetag` line, integer/float/double args, and
// `#bundle` packets recursively. Anything outside that minimal slice is
// ignored (logged at Debug level).

#include "entity/plugin/PluginContext.hpp"

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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using entity::plugin::IPluginContext;
using entity::plugin::LogLevel;

constexpr std::uint16_t kDefaultPort = 53000;

// Singleton listener state. The plugin is registered once; we treat its
// state as a process-global the way bus-logger treats its log sink.
struct ReceiverState {
    std::atomic<bool> running{false};
    socket_t          socket{ENTITY_OSC_INVALID};
    std::thread       worker;
    IPluginContext*   ctx{nullptr};
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
// OSC 1.0 wire-format helpers. Everything is big-endian on the wire.

std::uint32_t readU32BE(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) |
           (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8)  |
            std::uint32_t(p[3]);
}

std::int32_t readI32BE(const std::uint8_t* p) {
    return static_cast<std::int32_t>(readU32BE(p));
}

float readF32BE(const std::uint8_t* p) {
    std::uint32_t bits = readU32BE(p);
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

std::int64_t readI64BE(const std::uint8_t* p) {
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | p[i];
    return static_cast<std::int64_t>(bits);
}

double readF64BE(const std::uint8_t* p) {
    std::uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | p[i];
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
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
// JSON snippet builders for paramsJson. Tiny stack-buffered formatters —
// none of the commands we route take structured arguments.

void buildIntJson(std::string& out, std::string_view key, std::int64_t v) {
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "{\"%.*s\":%lld}",
                          int(key.size()), key.data(),
                          static_cast<long long>(v));
    out.assign(buf, n > 0 ? std::size_t(n) : 0u);
}

void buildDoubleJson(std::string& out, std::string_view key, double v) {
    char buf[64];
    int n = std::snprintf(buf, sizeof(buf), "{\"%.*s\":%.10g}",
                          int(key.size()), key.data(), v);
    out.assign(buf, n > 0 ? std::size_t(n) : 0u);
}

bool parseDouble(std::string_view s, double& out) {
    if (s.empty()) return false;
    std::string buf(s);
    char* end = nullptr;
    out = std::strtod(buf.c_str(), &end);
    return end != buf.c_str();
}

// ---------------------------------------------------------------------------
// Address routing.

bool firstNumericArgAsInt64(std::string_view typeTag,
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

void dispatch(std::string_view address, std::string_view typeTag,
              const std::uint8_t* argsBegin, const std::uint8_t* argsEnd) {
    auto* ctx = state().ctx;
    if (!ctx) return;

    if (address == "/entity/play") {
        ctx->enqueueCommand("Play", {});
        return;
    }
    if (address == "/entity/pause") {
        ctx->enqueueCommand("Pause", {});
        return;
    }
    if (address == "/entity/stop") {
        ctx->enqueueCommand("Pause", {});
        ctx->enqueueCommand("SeekToFrame", "{\"frame\":0}");
        return;
    }
    if (address == "/entity/section/next") {
        ctx->enqueueCommand("SectionGo", {});
        return;
    }
    if (address == "/entity/seek") {
        std::int64_t frame = 0;
        if (firstNumericArgAsInt64(typeTag, argsBegin, argsEnd, frame)) {
            std::string body;
            buildIntJson(body, "frame", frame);
            ctx->enqueueCommand("SeekToFrame", body);
        } else {
            pluginLog(LogLevel::Warn,
                      "/entity/seek expects a numeric (i/h/f/d) frame arg");
        }
        return;
    }

    // /entity/cue/{number}/go
    constexpr std::string_view kCuePrefix = "/entity/cue/";
    constexpr std::string_view kCueSuffix = "/go";
    if (address.size() > kCuePrefix.size() + kCueSuffix.size() &&
        address.substr(0, kCuePrefix.size()) == kCuePrefix &&
        address.substr(address.size() - kCueSuffix.size()) == kCueSuffix) {
        std::string_view numStr = address.substr(
            kCuePrefix.size(),
            address.size() - kCuePrefix.size() - kCueSuffix.size());
        double number = 0.0;
        if (parseDouble(numStr, number)) {
            std::string body;
            buildDoubleJson(body, "number", number);
            ctx->enqueueCommand("FireCue", body);
        } else {
            std::string m = "/entity/cue/.../go: bad number '";
            m.append(numStr); m.push_back('\'');
            pluginLog(LogLevel::Warn, m);
        }
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
        if (len < 16) return;     // need 8-byte tag + 8-byte timetag
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
        // OSC 1.0 also permits no type-tag (legacy); treat as empty.
        typeTag = std::string_view();
        // pos is unchanged; arg span is the rest of the packet.
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
        if (err == WSAETIMEDOUT) continue;          // expected, lets us re-check running
        if (err == WSAEINTR) continue;              // socket closed by shutdown
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

    // 250ms recv timeout so the worker re-checks the running flag and
    // exits promptly when the engine signals shutdown.
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

// Engine-driven shutdown hook. Joins the worker BEFORE the engine starts
// tearing down dispatcher/bus — see the comment on
// IPluginContext::registerShutdownHook for why this matters.
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

// Settings precedence: ENTITY_OSC_PORT env var > Settings.oscReceiverPort >
// the kDefaultPort built-in. The env var stays as a developer escape hatch
// so test scripts can run without touching settings.json; Preferences is
// the normal user-facing path.
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
        // Successful registration; the listener simply does not bind. The
        // shutdown hook isn't installed because there is nothing to join.
        // Clear ctx so the (unused) static state doesn't dangle.
        state().ctx = nullptr;
        return 0;
    }

    if (!startListener(resolvePort(ctx))) {
        state().ctx = nullptr;
        return -3;
    }

    // Engine shuts us down before tearing the dispatcher / bus.
    ctx->registerShutdownHook(&entity_plugin_osc_receiver_shutdown);
    return 0;
}
