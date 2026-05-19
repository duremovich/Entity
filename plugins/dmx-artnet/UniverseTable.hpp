// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace entity::dmx {

// Distinguishes the source of a DMX frame for arbitration + diagnostics.
// Art-Net + sACN + Enttec all feed the same UniverseTable; the tag lets
// us notice mid-frame when sACN takes over from Art-Net, etc.
enum class SourceTag : std::uint8_t {
    Unknown = 0,
    Artnet  = 1,
    Sacn    = 2,
    Enttec  = 3,
};

constexpr std::size_t kChannelsPerUniverse = 512;

// One universe's-worth of channel state, plus the per-frame metadata the
// arbiter needs to decide whether the next incoming packet wins.
struct Universe {
    std::array<std::uint8_t, kChannelsPerUniverse> channels{};
    std::array<std::uint8_t, kChannelsPerUniverse> lastChannels{};
    SourceTag                                       source{SourceTag::Unknown};
    std::uint8_t                                    priority{0};
    std::chrono::steady_clock::time_point           lastUpdate{};
    // True after the first packet for this universe; before then
    // lastChannels is meaningless (don't fire threshold edges on
    // first-ever observation).
    bool                                            primed{false};
};

// Called by mapping evaluation when a channel crosses an edge or
// changes (rules belong to the mapping table — UniverseTable just
// hands raw before/after snapshots over).
//
// universe in [0, 32767]; the table walks all changed channels in
// the freshly-applied frame.
using ChangeCallback = std::function<void(std::uint16_t universe,
                                          const std::uint8_t* before,
                                          const std::uint8_t* after)>;

// Process-wide arbitration map. One instance owned by the plugin.
// Mutex-guarded; ingress threads call apply() concurrently. Apply does
// arbitration first, then (if accepted) invokes the change callback
// while still holding the lock so mapping fire order is deterministic
// across threads.
class UniverseTable {
public:
    UniverseTable() = default;
    UniverseTable(const UniverseTable&) = delete;
    UniverseTable& operator=(const UniverseTable&) = delete;

    // Set once by the plugin during register; called from any ingress
    // thread on every accepted frame. Callback fires under the table
    // mutex so don't do anything heavy in it (enqueueCommand is fine
    // — that lock is independent and short).
    void setChangeCallback(ChangeCallback cb) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_callback = std::move(cb);
    }

    // Apply a freshly-decoded DMX frame for `universe` from `source` at
    // `priority`. Returns true if the frame was accepted into the table
    // and the change callback was invoked.
    //
    // Arbitration rules:
    //   - If table is empty for this universe -> accept.
    //   - If new priority > stored priority   -> accept.
    //   - If new priority < stored priority   -> reject.
    //   - If equal priority and same source   -> accept (continuous).
    //   - If equal priority, different source, AND stored is >2.5s old
    //                                          -> accept (takeover).
    //   - Otherwise                            -> reject.
    bool apply(std::uint16_t              universe,
               const std::uint8_t*        channels,
               std::size_t                count,
               SourceTag                  source,
               std::uint8_t               priority,
               std::chrono::steady_clock::time_point now);

private:
    static constexpr std::chrono::milliseconds kDataLossTimeout{2500};

    mutable std::mutex                                 m_mutex;
    std::unordered_map<std::uint16_t, Universe>        m_universes;
    ChangeCallback                                     m_callback;
};

} // namespace entity::dmx
