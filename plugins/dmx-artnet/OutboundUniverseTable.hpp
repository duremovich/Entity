// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "UniverseTable.hpp" // for kChannelsPerUniverse

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace entity::dmx {

// Writable per-universe table for outbound DMX. Two writer paths:
//   - setChannel(universe, channel, value, priority)  -- from SetDmxOut
//   - applyFrame(universe, span, priority)            -- from DmxFrame bus
//
// Both paths arbitrate per-channel by priority: a higher-priority write
// overrides; equal-priority replaces. Phase 5 might add per-priority
// staging, but v1 is the simple "last highest wins" model.
class OutboundUniverseTable {
public:
    struct UniverseSlot {
        std::array<std::uint8_t,  kChannelsPerUniverse> channels{};
        std::array<std::uint8_t,  kChannelsPerUniverse> priorities{};
        bool active{false};
    };

    void setChannel(std::uint16_t universe, std::uint16_t channelOneBased,
                    std::uint8_t value, std::uint8_t priority);

    void applyFrame(std::uint16_t universe,
                    const std::uint8_t* channels, std::size_t count,
                    std::uint8_t priority);

    // Snapshot all currently-active universes for the outbound sender
    // to emit. Returns pairs of (universe, slot).
    std::vector<std::pair<std::uint16_t, UniverseSlot>> snapshot() const;

private:
    mutable std::mutex                              m_mutex;
    std::unordered_map<std::uint16_t, UniverseSlot> m_universes;
};

} // namespace entity::dmx
