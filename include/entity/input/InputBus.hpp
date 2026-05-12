#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace entity {

/**
 * InputBus — cross-system named-value input channel registry.
 *
 * Input sources (OSC plugins, the editor keyboard handler, MIDI plugins,
 * future audio analyzers, script commands) write to named channels via
 * setFloat. Gameplay systems (GenerativeSystem etc.) read those channels
 * via getFloat during their per-tick update on the editor thread.
 *
 * Threading: writes can come from any thread (OSC plugin worker, GLFW
 * main thread, script driver) — channel values are stored as atomic
 * floats so reads/writes don't tear. The channel map itself is protected
 * by a mutex (rare-allocation slow path). Channel `shared_ptr`s are
 * never removed, so a cached pointer stays valid for the lifetime of
 * the bus.
 *
 * Channel-name convention: namespaced "<system>.<scope>.<axis>", e.g.
 *   muncher.input.x       — Muncher player X axis, [-1, 1]
 *   muncher.input.y       — Muncher player Y axis, [-1, 1]
 *   muncher.input.reset   — pulse (0 → 1 → 0) to restart the level
 *   (future) audio.kick   — audio analyzer kick amplitude, [0, 1]
 *
 * Unknown channels read as `fallback` — gameplay code never crashes on a
 * missing wire. New input sources are additive: a channel that no plugin
 * writes simply reads 0 forever, and the system using it falls back to
 * its default. Same forward-compat property the bus messages have.
 */
class InputBus {
public:
    InputBus() = default;
    InputBus(const InputBus&)            = delete;
    InputBus& operator=(const InputBus&) = delete;

    // Write the named float channel. Allocates the channel slot on first
    // use; subsequent writes hit only the atomic. Thread-safe.
    void setFloat(std::string_view name, float v);

    // Read the named float channel, returning `fallback` if no source has
    // ever written it. Thread-safe.
    float getFloat(std::string_view name, float fallback = 0.0f) const;

private:
    struct Channel {
        std::atomic<float> value{0.0f};
    };

    mutable std::mutex                                       m_mutex;
    std::unordered_map<std::string, std::shared_ptr<Channel>> m_channels;
};

} // namespace entity
