// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include <cstdint>

// Process-wide bridge between the engine (which knows when to emit DMX
// -- script commands, future timeline cells) and the dmx-artnet plugin
// (which owns the outbound socket + send thread).
//
// Why a function-pointer hook rather than an entity-bus message: see
// ADR-0023. The two-channel D2R/R2D bus serves Director<->Renderer,
// not engine<->plugin; adding a third direction would expand the bus's
// scope for a single feature. A function-pointer hook is a one-line
// indirection that keeps the engine free of plugin types and lets the
// plugin own all the state.
//
// When the plugin isn't loaded or is disabled in Preferences, calls
// into the bridge are silent no-ops.

namespace entity::dmx {

class OutboundBridge {
public:
    using SetChannelFn = void (*)(std::uint16_t universe,
                                   std::uint16_t channelOneBased,
                                   std::uint8_t  value,
                                   std::uint8_t  priority);

    static OutboundBridge& instance();

    // Called by the plugin on register/shutdown. nullptr unwires.
    void setHook(SetChannelFn hook) noexcept;

    // Called by the engine (SetDmxOutCommand). Silent no-op when
    // setHook hasn't been called.
    void setChannel(std::uint16_t universe,
                    std::uint16_t channelOneBased,
                    std::uint8_t  value,
                    std::uint8_t  priority = 100) noexcept;

private:
    OutboundBridge() = default;
};

} // namespace entity::dmx
