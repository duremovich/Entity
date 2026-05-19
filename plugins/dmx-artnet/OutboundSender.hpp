// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "OutboundUniverseTable.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  using send_socket_t = SOCKET;
#else
  using send_socket_t = int;
#endif

namespace entity::dmx {

// Configures + drives the outbound DMX send loop. Owned by the plugin;
// started from register if `dmxOutEnabled` is true, joined on shutdown.
class OutboundSender {
public:
    struct Config {
        bool                       artnetEnabled{false};
        bool                       sacnEnabled{false};
        std::vector<std::string>   artnetTargets; // dotted-quad strings; empty -> broadcast
        std::uint8_t               sacnPriority{100};
        // Sender's own sequence counter (Art-Net + sACN both increment
        // independently per packet — we use one counter for both).
    };

    OutboundSender(OutboundUniverseTable* table, Config cfg)
        : m_table(table), m_cfg(std::move(cfg)) {}
    ~OutboundSender() { stop(); }

    // Return false if the socket couldn't bind or the config is empty.
    bool start();
    void stop();

private:
    void                   workerLoop();
    void                   sendArtnet(std::uint16_t universe,
                                      const std::uint8_t* channels);
    void                   sendSacn(std::uint16_t universe,
                                    const std::uint8_t* channels);

    OutboundUniverseTable* m_table{nullptr};
    Config                 m_cfg;

    std::atomic<bool>      m_running{false};
    std::thread            m_worker;
    send_socket_t          m_socket{};

    // ArtDmx / sACN sequence counters. Wrap at 256 / 256 respectively
    // — both protocols use 8-bit sequence; receivers tolerate skips.
    std::atomic<std::uint8_t> m_artnetSeq{0};
    std::atomic<std::uint8_t> m_sacnSeq{0};

    // CID (RFC 4122 v4 UUID, sACN E1.31 section 5.6) — generated once
    // per process at start(). Receivers use this to de-duplicate
    // multiple senders.
    std::uint8_t           m_cid[16]{};
};

} // namespace entity::dmx
