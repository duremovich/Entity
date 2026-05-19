// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "OutboundSender.hpp"

#include <cstdio>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  static constexpr send_socket_t kInvalidSocket = INVALID_SOCKET;
  inline int close_send_socket(send_socket_t s) { return ::closesocket(s); }
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  static constexpr send_socket_t kInvalidSocket = -1;
  inline int close_send_socket(send_socket_t s) { return ::close(s); }
#endif

#if defined(TRACY_ENABLE)
  #include <tracy/Tracy.hpp>
#endif

#include <chrono>
#include <cstring>
#include <random>

namespace entity::dmx {

namespace {

constexpr std::uint16_t kArtnetPort = 6454;
constexpr std::uint16_t kSacnPort   = 5568;
constexpr auto          kTickPeriod = std::chrono::milliseconds(22); // ~44 Hz

void generateCid(std::uint8_t out[16]) {
    std::random_device rd;
    std::mt19937_64    rng(rd());
    std::uint64_t hi = rng();
    std::uint64_t lo = rng();
    std::memcpy(out,     &hi, 8);
    std::memcpy(out + 8, &lo, 8);
    // Set version (4) + variant (RFC 4122).
    out[6] = static_cast<std::uint8_t>((out[6] & 0x0F) | 0x40);
    out[8] = static_cast<std::uint8_t>((out[8] & 0x3F) | 0x80);
}

} // namespace

bool OutboundSender::start() {
    if (!m_cfg.artnetEnabled && !m_cfg.sacnEnabled) return false;
    if (m_running.exchange(true)) return false;

    generateCid(m_cid);

    m_socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (m_socket == kInvalidSocket) {
        m_running.store(false);
        return false;
    }

    // Enable broadcast permission only when targets is empty (the
    // Art-Net broadcast fallback). Setting SO_BROADCAST on Windows
    // changes how the kernel routes unicast loopback packets and
    // breaks our test sniffer; leave it off for explicit-target
    // sends.
    bool needBroadcast = m_cfg.artnetTargets.empty();
    if (needBroadcast) {
#ifdef _WIN32
        BOOL on = TRUE;
        ::setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char*>(&on), sizeof(on));
#else
        int on = 1;
        ::setsockopt(m_socket, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
#endif
    }

    // Outbound socket: no explicit bind. Let the OS pick the source
    // address + port at first sendto. We tried INADDR_ANY:0 with
    // SO_REUSEADDR but that produced unreliable loopback delivery on
    // Windows when a test process was listening on the destination
    // port. Equivalent code without the explicit bind defers source
    // selection to the kernel and works the same way on macOS/Linux.

    // Increase SO_SNDBUF so a burst of universes-per-tick doesn't get
    // backpressured on Windows when several outputs are configured.
#ifdef _WIN32
    int sndbuf = 256 * 1024;
    ::setsockopt(m_socket, SOL_SOCKET, SO_SNDBUF,
                 reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
#endif

    m_worker = std::thread(&OutboundSender::workerLoop, this);
    return true;
}

void OutboundSender::stop() {
    if (!m_running.exchange(false)) return;
    if (m_worker.joinable()) m_worker.join();
    if (m_socket != kInvalidSocket) {
        close_send_socket(m_socket);
        m_socket = kInvalidSocket;
    }
}

void OutboundSender::workerLoop() {
#if defined(TRACY_ENABLE)
    tracy::SetThreadName("DMX Out");
#endif
    while (m_running.load(std::memory_order_acquire)) {
        const auto next = std::chrono::steady_clock::now() + kTickPeriod;
        if (m_table) {
            auto snap = m_table->snapshot();
            for (const auto& [universe, slot] : snap) {
                if (m_cfg.artnetEnabled) sendArtnet(universe, slot.channels.data());
                if (m_cfg.sacnEnabled)   sendSacn(universe,   slot.channels.data());
            }
        }
        std::this_thread::sleep_until(next);
    }
}

void OutboundSender::sendArtnet(std::uint16_t universe,
                                 const std::uint8_t* channels) {
    constexpr std::size_t kHeader = 18;
    constexpr std::size_t kPayload = 512;
    std::uint8_t pkt[kHeader + kPayload];
    std::memcpy(pkt, "Art-Net\0", 8);
    pkt[8]  = 0x00;        // opcode LSB
    pkt[9]  = 0x50;        // opcode MSB -> 0x5000 ArtDmx
    pkt[10] = 0x00;        // protocol version high
    pkt[11] = 14;          // protocol version low
    pkt[12] = m_artnetSeq.fetch_add(1) + 1; // 1..255 then wrap; 0 = disabled
    pkt[13] = 0;           // physical
    pkt[14] = static_cast<std::uint8_t>(universe & 0xFF);
    pkt[15] = static_cast<std::uint8_t>((universe >> 8) & 0x7F);
    pkt[16] = 0x02;        // length high -> 512
    pkt[17] = 0x00;
    std::memcpy(pkt + kHeader, channels, kPayload);

    // Send to each configured target; empty list -> broadcast.
    std::vector<std::string> targets = m_cfg.artnetTargets;
    if (targets.empty()) targets.push_back("255.255.255.255");

    for (const auto& host : targets) {
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(kArtnetPort);
        if (::inet_pton(AF_INET, host.c_str(), &dst.sin_addr) != 1) {
            std::fprintf(stderr,
                          "[dmx-artnet] outbound: bad target IP '%s'\n",
                          host.c_str());
            continue;
        }
        const int sent = ::sendto(m_socket,
                                   reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
                                   reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
        if (sent < 0) {
#ifdef _WIN32
            int err = ::WSAGetLastError();
            std::fprintf(stderr,
                          "[dmx-artnet] outbound: sendto %s failed WSA=%d\n",
                          host.c_str(), err);
#else
            std::fprintf(stderr,
                          "[dmx-artnet] outbound: sendto %s failed\n",
                          host.c_str());
#endif
        }
    }
}

void OutboundSender::sendSacn(std::uint16_t universe,
                               const std::uint8_t* channels) {
    constexpr std::size_t kSize = 638; // standard E1.31 DMX packet size
    std::uint8_t pkt[kSize];
    std::memset(pkt, 0, sizeof(pkt));

    // --- Root layer ---
    pkt[0] = 0x00; pkt[1] = 0x10;  // preamble
    pkt[2] = 0x00; pkt[3] = 0x00;  // postamble
    std::memcpy(pkt + 4, "ASC-E1.17\0\0\0", 12);
    // Flags+length: 0x7 nybble + 0x26E (length 638 - 16 - 8?)
    // For a 512-channel data packet the canonical value is 0x726E.
    pkt[16] = 0x72; pkt[17] = 0x6E;
    // Vector
    pkt[18] = 0x00; pkt[19] = 0x00; pkt[20] = 0x00; pkt[21] = 0x04;
    // CID
    std::memcpy(pkt + 22, m_cid, 16);

    // --- Framing layer (offset 38) ---
    pkt[38] = 0x72; pkt[39] = 0x58;
    pkt[40] = 0x00; pkt[41] = 0x00; pkt[42] = 0x00; pkt[43] = 0x02;
    // Source name: zero-filled is fine
    pkt[108] = m_cfg.sacnPriority;
    pkt[109] = 0; pkt[110] = 0; // sync universe
    pkt[111] = m_sacnSeq.fetch_add(1);
    pkt[112] = 0;               // options
    pkt[113] = static_cast<std::uint8_t>((universe >> 8) & 0xFF);
    pkt[114] = static_cast<std::uint8_t>(universe & 0xFF);

    // --- DMP layer (offset 115) ---
    pkt[115] = 0x72; pkt[116] = 0x0B;
    pkt[117] = 0x02;
    pkt[118] = 0xA1;
    pkt[119] = 0x00; pkt[120] = 0x00; // first prop address
    pkt[121] = 0x00; pkt[122] = 0x01; // increment
    pkt[123] = 0x02; pkt[124] = 0x01; // property value count = 513
    pkt[125] = 0x00;                  // DMX start code
    std::memcpy(pkt + 126, channels, 512);

    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(kSacnPort);
    std::uint8_t addr[4] = {239, 255,
                             static_cast<std::uint8_t>((universe >> 8) & 0xFF),
                             static_cast<std::uint8_t>(universe & 0xFF)};
    std::memcpy(&dst.sin_addr.s_addr, addr, 4);
    ::sendto(m_socket, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0,
             reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
}

} // namespace entity::dmx
