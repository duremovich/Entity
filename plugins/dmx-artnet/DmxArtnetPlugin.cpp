// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#if defined(TRACY_ENABLE)
#  include <tracy/Tracy.hpp>
#endif
//
// dmx-artnet -- DMX-512 I/O over Art-Net (UDP 6454), sACN/E1.31
// (UDP 5568 multicast), and Enttec DMX-USB-Pro (FTDI serial, Windows).
//
// Architecture:
//
//   Three inbound worker threads (Art-Net, sACN, Enttec) all feed one
//   process-wide UniverseTable. Mapping evaluation fires synchronously
//   under the table's mutex so triggers are deterministic. Commands
//   route via IPluginContext::enqueueCommand per ADR-0013.
//
//   One outbound worker thread (OutboundSender) drains an
//   OutboundUniverseTable at ~44 Hz and emits Art-Net + sACN packets.
//   Writers into the outbound table are the engine's SetDmxOutCommand
//   via OutboundBridge (ADR-0023).
//
//   Phase 5 will wire MappingResolver to read project-scoped mappings;
//   today it returns the baked defaults from DefaultMappings.cpp.
//
//   Engine-driven shutdown hook joins every worker before the engine
//   starts tearing down the dispatcher / bus / settings.

#include "entity/plugin/PluginContext.hpp"

#include "entity/dmx/OutboundBridge.hpp"

#include "ArtnetParser.hpp"
#include "CommandEmitter.hpp"
#include "DefaultMappings.hpp"
#include "MappingResolver.hpp"
#include "MulticastJoin.hpp"
#include "OutboundSender.hpp"
#include "OutboundUniverseTable.hpp"
#include "SacnParser.hpp"
#include "UniverseTable.hpp"

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
  static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
  inline int closeSocket(socket_t s) { return ::closesocket(s); }
  #include "EnttecFrameParser.hpp"
  #include "EnttecSerial.hpp"
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_t = int;
  static constexpr socket_t kInvalidSocket = -1;
  inline int closeSocket(socket_t s) { return ::close(s); }
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using entity::plugin::IPluginContext;
using entity::plugin::LogLevel;

// One process-wide block of plugin state. The plugin is registered once
// per engine boot; treat it as a singleton like the OSC plugin does.
struct PluginState {
    IPluginContext*                                ctx{nullptr};
    std::atomic<bool>                              running{false};

    // Inbound — shared by all three ingress threads.
    entity::dmx::UniverseTable                     table;
    entity::dmx::MappingResolver                   resolver;

    // Art-Net listener
    socket_t                                       artnetSocket{kInvalidSocket};
    std::thread                                    artnetThread;

    // sACN listener
    socket_t                                       sacnSocket{kInvalidSocket};
    std::thread                                    sacnThread;
    entity::dmx::MulticastMembership               sacnMembership;

#ifdef _WIN32
    // Enttec serial
    void*                                          enttecPort{nullptr}; // HANDLE
    std::thread                                    enttecThread;
#endif

    // Outbound
    entity::dmx::OutboundUniverseTable             outbound;
    std::unique_ptr<entity::dmx::OutboundSender>   sender;

    bool                                           winsockUp{false};
};

PluginState& state() {
    static PluginState s;
    return s;
}

void pluginLog(LogLevel level, const std::string& msg) {
    auto* ctx = state().ctx;
    if (ctx) ctx->log(level, msg);
}

void pluginLog(LogLevel level, const char* msg) {
    auto* ctx = state().ctx;
    if (ctx) ctx->log(level, msg);
}

// -----------------------------------------------------------------------------
// Outbound hook -- installed via OutboundBridge so SetDmxOutCommand can reach
// the plugin's outbound table without the engine depending on plugin types.
// -----------------------------------------------------------------------------
void outboundHook(std::uint16_t universe,
                  std::uint16_t channelOneBased,
                  std::uint8_t  value,
                  std::uint8_t  priority) {
    state().outbound.setChannel(universe, channelOneBased, value, priority);
}

// -----------------------------------------------------------------------------
// UniverseTable change callback -- fires mappings.
// -----------------------------------------------------------------------------
void onUniverseChange(std::uint16_t universe,
                      const std::uint8_t* before,
                      const std::uint8_t* after) {
    auto* ctx = state().ctx;
    if (!ctx) return;
    // Re-read the project's mapping table. Cheap when unchanged
    // (the resolver memoizes the last JSON blob and short-circuits
    // on identity). Picks up project loads + mapping edits without
    // any cross-thread notification path.
    state().resolver.refreshFromProject(ctx);
    const auto mappings = state().resolver.current();
    entity::dmx::emitMappingFires(ctx, mappings, universe, before, after,
                                   "inbound");
}

// -----------------------------------------------------------------------------
// Socket helpers
// -----------------------------------------------------------------------------
bool ensureWinsock() {
#ifdef _WIN32
    if (state().winsockUp) return true;
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    state().winsockUp = true;
#endif
    return true;
}

socket_t bindUdp(std::uint16_t port, bool reuseAddr) {
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket) return s;

    if (reuseAddr) {
#ifdef _WIN32
        BOOL reuse = TRUE;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
        int reuse = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    }

    // Larger receive buffer so the listener can absorb burst traffic
    // (a lighting console firing several universes per frame, or
    // initial scene reload). Default Windows UDP recv buffer is
    // ~8 KB; bump to 1 MB to give the editor headroom before the
    // worker thread's next wake-up.
#ifdef _WIN32
    int rcvbuf = 1024 * 1024;
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF,
                 reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
#else
    int rcvbuf = 1024 * 1024;
    ::setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSocket(s);
        return kInvalidSocket;
    }

    // 250 ms recv timeout so the worker re-checks the running flag.
#ifdef _WIN32
    DWORD tmo = 250;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&tmo), sizeof(tmo));
#else
    timeval tv{};
    tv.tv_sec  = 0;
    tv.tv_usec = 250'000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    return s;
}

// -----------------------------------------------------------------------------
// Art-Net worker thread
// -----------------------------------------------------------------------------
void artnetWorkerLoop() {
#if defined(TRACY_ENABLE)
    tracy::SetThreadName("DMX Art-Net");
#endif
    auto& s = state();
    std::vector<std::uint8_t> buf(2048);
    while (s.running.load(std::memory_order_acquire)) {
        sockaddr_in from{};
#ifdef _WIN32
        int fromLen = sizeof(from);
#else
        socklen_t fromLen = sizeof(from);
#endif
        int n = ::recvfrom(s.artnetSocket,
                           reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()),
                           0,
                           reinterpret_cast<sockaddr*>(&from),
                           &fromLen);
        if (n <= 0) {
            if (!s.running.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        entity::dmx::ArtnetDmx parsed{};
        if (!entity::dmx::parseArtnetPacket(buf.data(),
                                             static_cast<std::size_t>(n),
                                             parsed)) {
            continue;  // not ArtDmx -- ArtPoll / vendor opcodes / noise
        }
        std::fprintf(stderr, "[dmx-artnet] artnet parsed u=%u ch=%u\n",
                      unsigned(parsed.universe), unsigned(parsed.channelCount));
        s.table.apply(parsed.universe, parsed.channels, parsed.channelCount,
                       entity::dmx::SourceTag::Artnet, /*priority*/100,
                       std::chrono::steady_clock::now());
    }
}

// -----------------------------------------------------------------------------
// sACN worker thread
// -----------------------------------------------------------------------------
void sacnWorkerLoop() {
#if defined(TRACY_ENABLE)
    tracy::SetThreadName("DMX sACN");
#endif
    auto& s = state();
    // Join multicast groups for active universes.
    s.sacnMembership.resync(s.sacnSocket, s.resolver.activeUniverses());

    std::vector<std::uint8_t> buf(2048);
    while (s.running.load(std::memory_order_acquire)) {
        sockaddr_in from{};
#ifdef _WIN32
        int fromLen = sizeof(from);
#else
        socklen_t fromLen = sizeof(from);
#endif
        int n = ::recvfrom(s.sacnSocket,
                           reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()),
                           0,
                           reinterpret_cast<sockaddr*>(&from),
                           &fromLen);
        if (n <= 0) {
            if (!s.running.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        entity::dmx::SacnDmx parsed{};
        if (!entity::dmx::parseSacnPacket(buf.data(),
                                            static_cast<std::size_t>(n),
                                            parsed)) {
            continue;
        }
        s.table.apply(parsed.universe, parsed.channels, parsed.channelCount,
                       entity::dmx::SourceTag::Sacn, parsed.priority,
                       std::chrono::steady_clock::now());
    }

    s.sacnMembership.leaveAll(s.sacnSocket);
}

// -----------------------------------------------------------------------------
// Enttec worker thread (Windows-only)
// -----------------------------------------------------------------------------
#ifdef _WIN32
void enttecWorkerLoop(std::uint16_t universe) {
#if defined(TRACY_ENABLE)
    tracy::SetThreadName("DMX Enttec");
#endif
    auto& s = state();
    if (s.enttecPort == nullptr) return;

    // Tell the Pro to send DMX-on-change frames (label 8 payload [1,0,0]).
    HANDLE h = static_cast<HANDLE>(s.enttecPort);
    {
        const std::uint8_t enable[] = {0x7E, 8, 3, 0, 1, 0, 0, 0xE7};
        DWORD written = 0;
        ::WriteFile(h, enable, sizeof(enable), &written, nullptr);
    }

    entity::dmx::EnttecFrameParser parser([universe, &s](const std::uint8_t* ch,
                                                          std::size_t count) {
        s.table.apply(universe, ch, count,
                       entity::dmx::SourceTag::Enttec, /*priority*/100,
                       std::chrono::steady_clock::now());
    });

    std::uint8_t buf[512];
    while (s.running.load(std::memory_order_acquire)) {
        DWORD got = 0;
        if (!::ReadFile(h, buf, sizeof(buf), &got, nullptr)) {
            // Port likely closed -- bail.
            break;
        }
        if (got > 0) parser.feed(buf, got);
    }
}
#endif

// -----------------------------------------------------------------------------
// Settings helpers (strings via env fallback like OSC does)
// -----------------------------------------------------------------------------
std::uint16_t resolvePort(IPluginContext* ctx, const char* settingsKey,
                          const char* envVar, std::uint16_t builtin) {
    if (envVar) {
        if (const char* env = std::getenv(envVar)) {
            int v = std::atoi(env);
            if (v > 0 && v < 65536) return static_cast<std::uint16_t>(v);
        }
    }
    int port = ctx->getIntSetting(settingsKey, builtin);
    if (port < 1 || port > 65535) port = builtin;
    return static_cast<std::uint16_t>(port);
}

// Split a comma-separated string of Art-Net target IPs.
std::vector<std::string> splitTargets(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        // Trim whitespace.
        auto a = tok.find_first_not_of(" \t");
        auto b = tok.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        out.emplace_back(tok.substr(a, b - a + 1));
    }
    return out;
}

// -----------------------------------------------------------------------------
// Engine-driven shutdown
// -----------------------------------------------------------------------------
extern "C" void entity_plugin_dmx_artnet_shutdown() {
    auto& s = state();
    if (!s.running.exchange(false)) return;

    // Unwire outbound hook first so engine commands stop arriving.
    entity::dmx::OutboundBridge::instance().setHook(nullptr);

    // Stop outbound sender (joins its own thread).
    if (s.sender) {
        s.sender->stop();
        s.sender.reset();
    }

    // Close inbound sockets to unblock recvfrom.
    if (s.artnetSocket != kInvalidSocket) {
        closeSocket(s.artnetSocket);
        s.artnetSocket = kInvalidSocket;
    }
    if (s.sacnSocket != kInvalidSocket) {
        closeSocket(s.sacnSocket);
        s.sacnSocket = kInvalidSocket;
    }
#ifdef _WIN32
    if (s.enttecPort) {
        ::CloseHandle(static_cast<HANDLE>(s.enttecPort));
        s.enttecPort = nullptr;
    }
#endif

    if (s.artnetThread.joinable())  s.artnetThread.join();
    if (s.sacnThread.joinable())    s.sacnThread.join();
#ifdef _WIN32
    if (s.enttecThread.joinable())  s.enttecThread.join();
#endif

#ifdef _WIN32
    if (s.winsockUp) {
        ::WSACleanup();
        s.winsockUp = false;
    }
#endif

    if (s.ctx) s.ctx->log(LogLevel::Info, "dmx-artnet stopped");
    s.ctx = nullptr;
}

} // namespace

// -----------------------------------------------------------------------------
// Registration entry point
// -----------------------------------------------------------------------------
extern "C" int entity_plugin_register_dmx_artnet(IPluginContext* ctx) {
    if (ctx == nullptr) return -1;
    if (ctx->apiVersion() != entity::plugin::PLUGIN_API_VERSION) {
        ctx->log(LogLevel::Error,
                 std::string("API version mismatch: engine=") +
                 std::to_string(ctx->apiVersion()) +
                 " plugin=" +
                 std::to_string(entity::plugin::PLUGIN_API_VERSION));
        return -2;
    }

    auto& s = state();
    s.ctx = ctx;

    // Wire the universe-change callback so accepted frames fire
    // mappings on the ingress thread.
    s.table.setChangeCallback(&onUniverseChange);

    // Phase 5 hook -- harmless no-op today (resolver pinned to baked).
    s.resolver.refreshFromProject(ctx);

    const bool artnetEnabled = ctx->getBoolSetting("dmxArtnetEnabled", false);
    const bool sacnEnabled   = ctx->getBoolSetting("dmxSacnEnabled",   false);
    const bool outEnabled    = ctx->getBoolSetting("dmxOutEnabled",    false);
    const bool enttecEnabled = ctx->getBoolSetting("dmxEnttecEnabled", false);

    if (!artnetEnabled && !sacnEnabled && !outEnabled && !enttecEnabled) {
        ctx->log(LogLevel::Info,
                 "dmx-artnet: all surfaces disabled in Preferences -- no listeners or sender");
        s.ctx = nullptr;
        return 0;
    }

    if (!ensureWinsock()) {
        ctx->log(LogLevel::Error, "dmx-artnet: WSAStartup failed");
        s.ctx = nullptr;
        return -3;
    }

    s.running.store(true, std::memory_order_release);

    // -------- Art-Net inbound --------
    if (artnetEnabled) {
        const auto port = resolvePort(ctx, "dmxArtnetListenPort",
                                       "ENTITY_DMX_ARTNET_PORT", 6454);
        s.artnetSocket = bindUdp(port, /*reuseAddr*/true);
        if (s.artnetSocket == kInvalidSocket) {
            char m[128];
            std::snprintf(m, sizeof(m),
                           "dmx-artnet: bind UDP %u failed for Art-Net listener",
                           unsigned(port));
            ctx->log(LogLevel::Error, m);
        } else {
            char m[128];
            std::snprintf(m, sizeof(m),
                           "dmx-artnet: Art-Net listening on UDP 0.0.0.0:%u",
                           unsigned(port));
            ctx->log(LogLevel::Info, m);
            s.artnetThread = std::thread(&artnetWorkerLoop);
        }
    }

    // -------- sACN inbound --------
    if (sacnEnabled) {
        s.sacnSocket = bindUdp(5568, /*reuseAddr*/true);
        if (s.sacnSocket == kInvalidSocket) {
            ctx->log(LogLevel::Error,
                     "dmx-artnet: bind UDP 5568 failed for sACN listener");
        } else {
            ctx->log(LogLevel::Info,
                     "dmx-artnet: sACN listening on UDP 0.0.0.0:5568 (multicast)");
            s.sacnThread = std::thread(&sacnWorkerLoop);
        }
    }

    // -------- Enttec inbound (Windows-only) --------
#ifdef _WIN32
    if (enttecEnabled) {
        auto devices = entity::dmx::enumerateEnttecDevices();
        entity::dmx::writeDeviceSidecar(devices);

        // Pick port: configured name, or first device, or skip.
        std::string portKey; // empty -> auto
        (void)ctx->getIntSetting("dmxEnttecUniverse", 0); // touched, no logic v1

        std::string targetPort;
        // No string-setting accessor yet -- env var is the override path.
        if (const char* env = std::getenv("ENTITY_DMX_ENTTEC_PORT")) {
            targetPort = env;
        }
        if (targetPort.empty() && !devices.empty()) {
            targetPort = devices.front().portName;
        }
        if (targetPort.empty()) {
            ctx->log(LogLevel::Warn,
                     "dmx-artnet: Enttec enabled but no FTDI device found");
        } else {
            std::string fullName = std::string("\\\\.\\") + targetPort;
            HANDLE h = ::CreateFileA(fullName.c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                ctx->log(LogLevel::Error,
                         "dmx-artnet: Enttec CreateFile failed on " + targetPort);
            } else {
                DCB dcb{};
                dcb.DCBlength = sizeof(dcb);
                if (::GetCommState(h, &dcb)) {
                    dcb.BaudRate = 250000;
                    dcb.ByteSize = 8;
                    dcb.StopBits = TWOSTOPBITS;
                    dcb.Parity   = NOPARITY;
                    ::SetCommState(h, &dcb);
                }
                COMMTIMEOUTS tmo{};
                tmo.ReadIntervalTimeout         = 100;
                tmo.ReadTotalTimeoutConstant    = 250;
                tmo.ReadTotalTimeoutMultiplier  = 0;
                ::SetCommTimeouts(h, &tmo);

                s.enttecPort   = h;
                const auto universe = static_cast<std::uint16_t>(
                    ctx->getIntSetting("dmxEnttecUniverse", 0));
                ctx->log(LogLevel::Info,
                         "dmx-artnet: Enttec opened on " + targetPort);
                s.enttecThread = std::thread(&enttecWorkerLoop, universe);
            }
        }
    }
#else
    (void)enttecEnabled;
#endif

    // -------- Outbound sender --------
    if (outEnabled) {
        entity::dmx::OutboundSender::Config cfg;
        cfg.artnetEnabled = true; // Outbound flag enables both targets;
        cfg.sacnEnabled   = ctx->getBoolSetting("dmxOutSacnEnabled", false);

        // dmxOutArtnetTargets is a string setting -- no getStringSetting
        // in the API yet (lands in Phase 5). Use env override for v1.
        if (const char* env = std::getenv("ENTITY_DMX_OUT_TARGETS")) {
            cfg.artnetTargets = splitTargets(env);
        }

        s.sender = std::make_unique<entity::dmx::OutboundSender>(
            &s.outbound, std::move(cfg));
        if (!s.sender->start()) {
            ctx->log(LogLevel::Error,
                     "dmx-artnet: outbound sender failed to start");
            s.sender.reset();
        } else {
            ctx->log(LogLevel::Info,
                     "dmx-artnet: outbound sender running at 44 Hz");
            entity::dmx::OutboundBridge::instance().setHook(&outboundHook);
        }
    }

    // Engine shuts us down before tearing the dispatcher / bus.
    ctx->registerShutdownHook(&entity_plugin_dmx_artnet_shutdown);
    return 0;
}
