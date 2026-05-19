// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "MulticastJoin.hpp"

#ifdef _WIN32
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
#endif

#include <cstring>

namespace entity::dmx {

namespace {

void setMreq(ip_mreq& mreq, std::uint16_t universe) {
    std::memset(&mreq, 0, sizeof(mreq));
    // 239.255.high.low
    std::uint8_t addr[4] = {239, 255,
                             static_cast<std::uint8_t>((universe >> 8) & 0xFF),
                             static_cast<std::uint8_t>(universe & 0xFF)};
    std::memcpy(&mreq.imr_multiaddr.s_addr, addr, 4);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);
}

} // namespace

void MulticastMembership::resync(sacn_socket_t socket,
                                  const std::unordered_set<std::uint16_t>& universes) {
    // Leave groups that disappeared.
    for (auto it = m_joined.begin(); it != m_joined.end(); ) {
        if (universes.find(*it) == universes.end()) {
            ip_mreq mreq;
            setMreq(mreq, *it);
            ::setsockopt(socket, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                         reinterpret_cast<const char*>(&mreq), sizeof(mreq));
            it = m_joined.erase(it);
        } else {
            ++it;
        }
    }
    // Join newcomers.
    for (std::uint16_t u : universes) {
        if (m_joined.find(u) != m_joined.end()) continue;
        ip_mreq mreq;
        setMreq(mreq, u);
        if (::setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                         reinterpret_cast<const char*>(&mreq), sizeof(mreq)) == 0) {
            m_joined.insert(u);
        }
        // Failed joins (interface doesn't support IGMP, etc.) are
        // silently skipped — sACN sources sending unicast to 5568 still
        // reach us.
    }
}

void MulticastMembership::leaveAll(sacn_socket_t socket) {
    for (std::uint16_t u : m_joined) {
        ip_mreq mreq;
        setMreq(mreq, u);
        ::setsockopt(socket, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                     reinterpret_cast<const char*>(&mreq), sizeof(mreq));
    }
    m_joined.clear();
}

} // namespace entity::dmx
