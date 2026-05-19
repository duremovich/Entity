// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include <cstdint>
#include <unordered_set>

#ifdef _WIN32
  #include <winsock2.h>
  using sacn_socket_t = SOCKET;
#else
  using sacn_socket_t = int;
#endif

namespace entity::dmx {

// Diff-and-apply IGMP membership for sACN per-universe multicast
// groups. sACN multicast address for universe U is
// 239.255.<high(U)>.<low(U)>. Joining tens-of-thousands of groups is
// wasteful — keep the active set tight.
class MulticastMembership {
public:
    // Resync the joined-group set to exactly `universes`. Leaves groups
    // no longer in the set; joins newcomers. Idempotent.
    void resync(sacn_socket_t socket,
                const std::unordered_set<std::uint16_t>& universes);

    // Leave every group; used at shutdown.
    void leaveAll(sacn_socket_t socket);

private:
    std::unordered_set<std::uint16_t> m_joined;
};

} // namespace entity::dmx
