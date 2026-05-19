// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "UniverseTable.hpp"

#include <algorithm>
#include <cstring>

namespace entity::dmx {

bool UniverseTable::apply(std::uint16_t                          universe,
                          const std::uint8_t*                    channels,
                          std::size_t                            count,
                          SourceTag                              source,
                          std::uint8_t                           priority,
                          std::chrono::steady_clock::time_point  now) {
    if (channels == nullptr || count == 0) return false;
    count = std::min(count, kChannelsPerUniverse);

    std::lock_guard<std::mutex> lk(m_mutex);
    auto& slot = m_universes[universe];

    // Arbitrate. Phase 1 has only one source so this is effectively a
    // no-op — the structure lands now so Phase 2 (sACN priority) and
    // Phase 4 (Enttec takeover) don't have to retrofit it.
    const bool empty = (slot.source == SourceTag::Unknown);
    if (!empty) {
        if (priority < slot.priority) return false;
        if (priority == slot.priority && source != slot.source) {
            const auto age = now - slot.lastUpdate;
            if (age < kDataLossTimeout) return false;
        }
    }

    // Snapshot the previous channels so the callback can detect edges
    // even on the very first accepted frame from a new source.
    std::array<std::uint8_t, kChannelsPerUniverse> before = slot.channels;

    // Commit the new frame. Channels past `count` retain their previous
    // value — DMX-USB-Pro reports only the channels that changed since
    // last frame, so blanking the tail would erase unrelated state.
    std::memcpy(slot.channels.data(), channels, count);

    slot.lastChannels = before;
    slot.source       = source;
    slot.priority     = priority;
    slot.lastUpdate   = now;

    const bool wasPrimed = slot.primed;
    slot.primed = true;

    if (m_callback && wasPrimed) {
        m_callback(universe, before.data(), slot.channels.data());
    } else if (m_callback) {
        // First-ever frame for this universe: still invoke the callback
        // so mappings can fire from boot if the user happens to start
        // sending at a non-zero value. Treat the "before" as all-zero
        // so threshold-edge rules see the rising edge from 0 -> v.
        std::array<std::uint8_t, kChannelsPerUniverse> zero{};
        m_callback(universe, zero.data(), slot.channels.data());
    }
    return true;
}

} // namespace entity::dmx
