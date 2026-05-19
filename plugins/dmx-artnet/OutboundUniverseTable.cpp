// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "OutboundUniverseTable.hpp"

#include <algorithm>

namespace entity::dmx {

void OutboundUniverseTable::setChannel(std::uint16_t universe,
                                       std::uint16_t channelOneBased,
                                       std::uint8_t  value,
                                       std::uint8_t  priority) {
    if (channelOneBased == 0 || channelOneBased > kChannelsPerUniverse) return;
    const std::uint16_t idx = static_cast<std::uint16_t>(channelOneBased - 1);

    std::lock_guard<std::mutex> lk(m_mutex);
    auto& slot = m_universes[universe];
    if (priority < slot.priorities[idx]) return;
    slot.channels[idx]   = value;
    slot.priorities[idx] = priority;
    slot.active          = true;
}

void OutboundUniverseTable::applyFrame(std::uint16_t universe,
                                       const std::uint8_t* channels,
                                       std::size_t count,
                                       std::uint8_t priority) {
    if (channels == nullptr || count == 0) return;
    count = std::min(count, kChannelsPerUniverse);

    std::lock_guard<std::mutex> lk(m_mutex);
    auto& slot = m_universes[universe];
    for (std::size_t i = 0; i < count; ++i) {
        if (priority >= slot.priorities[i]) {
            slot.channels[i]   = channels[i];
            slot.priorities[i] = priority;
        }
    }
    slot.active = true;
}

std::vector<std::pair<std::uint16_t, OutboundUniverseTable::UniverseSlot>>
OutboundUniverseTable::snapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<std::pair<std::uint16_t, UniverseSlot>> out;
    out.reserve(m_universes.size());
    for (const auto& [u, slot] : m_universes) {
        if (slot.active) out.emplace_back(u, slot);
    }
    return out;
}

} // namespace entity::dmx
