// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#include "entity/dmx/OutboundBridge.hpp"

#include <atomic>

namespace entity::dmx {

namespace {
// std::atomic<fn*> so the plugin can wire/unwire while the engine
// thread is calling setChannel concurrently.
std::atomic<OutboundBridge::SetChannelFn> g_hook{nullptr};
} // namespace

OutboundBridge& OutboundBridge::instance() {
    static OutboundBridge inst;
    return inst;
}

void OutboundBridge::setHook(SetChannelFn hook) noexcept {
    g_hook.store(hook, std::memory_order_release);
}

void OutboundBridge::setChannel(std::uint16_t universe,
                                 std::uint16_t channelOneBased,
                                 std::uint8_t  value,
                                 std::uint8_t  priority) noexcept {
    auto* hook = g_hook.load(std::memory_order_acquire);
    if (hook == nullptr) return;
    hook(universe, channelOneBased, value, priority);
}

} // namespace entity::dmx
