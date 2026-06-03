// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "Plugin.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Forward declaration -- plugins that touch the bus link entity-bus
// separately and include the full header. Plugins that don't touch the bus
// pay nothing for this declaration.
namespace entity::bus {
class IMessageTransport;
}

namespace entity::plugin {

// Playback state mirroring entity::PlaybackState without pulling in
// core headers (boundary rule: no internal headers in plugin-api).
enum class TransportState : int { Stopped = 0, Playing = 1, Paused = 2 };

// Snapshot of transport + section + project state at the moment of the call.
// Returned by value — no caller-managed buffers, no pointers into engine
// internals. Safe to stash and diff on a plugin worker thread.
struct TransportSnapshot {
    TransportState playbackState{TransportState::Stopped};
    int64_t        frameNumber{0};
    double         frameRate{30.0};

    int     activeSectionIndex{-1};  // -1 if no sections
    int64_t activeSectionFrame{0};
    int     nextSectionIndex{-1};    // -1 if at end of show
    int64_t nextSectionFrame{0};

    std::string projectName;         // filename stem; empty if no project loaded
};

enum class LogLevel : int {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

// POD mirror of bus::SignalArg — primitives only so this Apache-2.0 header
// never needs to include the GPL bus/Message.hpp. The host converts
// bus::SignalEmit -> SignalEmitPod before handing it to plugins.
struct SignalArgPod {
    enum class Type : int { Int = 0, Float = 1, String = 2 };

    Type    type{Type::Int};
    int32_t i{0};
    float   f{0.0f};
    // Inline storage for the string value. Fixed-size avoids heap allocation
    // in the hot path; 128 bytes covers typical OSC string args.
    static constexpr std::size_t kMaxStringLen = 127;
    char    s[kMaxStringLen + 1]{};
};

// POD mirror of bus::SignalEmit. `address` is the OSC address pattern.
// `argCount` is the number of valid entries in `args`.
struct SignalEmitPod {
    // Address pattern, e.g. "/entity/signal/foo". NUL-terminated.
    static constexpr std::size_t kMaxAddressLen = 255;
    char        address[kMaxAddressLen + 1]{};

    // Argument list. Up to 8 typed args per signal (OSC practical limit).
    static constexpr std::size_t kMaxArgs = 8;
    SignalArgPod args[kMaxArgs]{};
    std::size_t  argCount{0};
};

// Handed to a plugin's register function at startup. Implemented by the
// engine; plugins consume the interface and must not subclass it.
//
// Lifetime: the IPluginContext* is valid from the moment register() is
// called until the engine begins shutdown. Plugins must not stash it past
// shutdown. If a plugin needs to publish bus messages from a worker thread,
// it should retain the bus() pointer (not the context itself) -- the
// transport is thread-safe by contract; the context is not.
//
// Boundary rule: every method here is noexcept, returns value-safe data
// (may include std::string and other STL types compatible across a
// statically-linked plugin boundary) or stable pointers, and never
// exposes core internals. New methods land at the bottom of the vtable
// so existing compiled plugins keep working across patch-version bumps.
class IPluginContext {
public:
    virtual ~IPluginContext() = default;

    // The PLUGIN_API_VERSION the engine was built against. A plugin may
    // assert this matches its own at register time and fail-fast if not.
    virtual int apiVersion() const noexcept = 0;

    // The engine's bus transport. Plugins use this to publish messages
    // (Direction::D2R for Director-bound, Direction::R2D for Renderer-
    // bound). Nullable in unusual configurations (e.g. a plugin loaded
    // by a tooling harness that doesn't run the bus); plugins must
    // tolerate a null return and log gracefully.
    virtual entity::bus::IMessageTransport* bus() noexcept = 0;

    // Diagnostic logging. The engine routes these to its own log sink and
    // prefixes the plugin name automatically.
    virtual void log(LogLevel level, std::string_view message) noexcept = 0;

    // Stable plugin-side identifier the engine assigned this plugin (the
    // target name from CMake, e.g. "bus-logger"). Useful for log prefixes
    // and for error messages a plugin emits about itself.
    virtual std::string_view pluginName() const noexcept = 0;

    // Enqueue a Director command from a plugin (control-plane shortcut).
    //
    // `typeName` is the dispatcher type string (e.g. "Play", "FireCue",
    // "SectionGo"); `paramsJson` is a UTF-8 JSON object literal of the
    // command's parameters (e.g. `{"number":1.5}`) or an empty view for
    // commands with no params. The implementation parses paramsJson and
    // forwards to `CommandDispatcher::enqueue(typeName, params)`.
    //
    // Returns true if the command type was recognized and queued. Returns
    // false on unknown command type, malformed JSON, or null bus/engine.
    // Thread-safe: the dispatcher's queue is mutex-guarded, so plugins may
    // call this from any worker thread.
    //
    // ABI note: this method lives at the bottom of the vtable so plugins
    // compiled against an older header still work — they just don't see
    // the new entry point. No PLUGIN_API_VERSION bump.
    virtual bool enqueueCommand(std::string_view typeName,
                                std::string_view paramsJson) noexcept = 0;

    // Install a callback the engine invokes at the start of its shutdown
    // sequence, before tearing down the bus, the dispatcher, or any other
    // subsystem the plugin might reach back into. Plugins that spawn worker
    // threads MUST register a hook that joins those threads here — otherwise
    // a worker holding the IPluginContext can race the destruction of the
    // engine and crash on use-after-free.
    //
    // Hooks fire in registration order. The function pointer must remain
    // valid until the engine has finished shutting down. Pass nullptr is a
    // no-op. Same ABI rule as enqueueCommand: bottom of the vtable, no
    // version bump.
    virtual void registerShutdownHook(PluginShutdownFn hook) noexcept = 0;

    // Read a primitive value from the engine's active Settings snapshot.
    // Stringly-typed because the GPL Settings struct can't be exposed
    // through this Apache-2.0 header (boundary rule). Unknown keys return
    // `defaultValue`; that lets a plugin compiled against a newer engine
    // ask for a setting an older engine doesn't have without breaking.
    //
    // Same ABI rule: bottom of the vtable, no PLUGIN_API_VERSION bump.
    virtual bool getBoolSetting(std::string_view key,
                                bool defaultValue) const noexcept = 0;
    virtual int  getIntSetting(std::string_view key,
                               int defaultValue) const noexcept = 0;

    // Read a string Setting (or a project-scoped state blob exposed via
    // the same accessor — special-cases certain keys to read from the
    // active project rather than the Settings struct):
    //   "dmxMappingsJson"        — active project's DMX mapping table JSON
    //   "oscInboundMappingsJson" — active project's OSC inbound mapping JSON
    //   "oscOutboundMappingsJson"— active project's OSC outbound mapping JSON
    //
    // Same ABI rule: bottom of vtable, no PLUGIN_API_VERSION bump.
    // The signature returns by value to keep the boundary copy-safe
    // (no caller-managed buffer lifetime).
    virtual std::string getStringSetting(std::string_view key,
                                         std::string_view defaultValue) const noexcept = 0;

    // Return a snapshot of transport + section + project state. Safe to
    // call from any plugin worker thread — the implementation takes brief
    // shared locks where needed and returns a fully-owned value struct.
    // Plugins must join any worker thread that calls this from within
    // their registerShutdownHook callback, so engine pointers remain
    // valid for the call's duration.
    //
    // Same ABI rule: bottom of vtable, no PLUGIN_API_VERSION bump.
    virtual TransportSnapshot getTransportSnapshot() const noexcept = 0;

    // Drain pending SignalEmit events into `out` (capacity `max`). Returns
    // the number of entries written (always <= max). Thread-safe: the
    // implementation guards its queue with a mutex so the osc-sender worker
    // thread may call this without racing the show thread's enqueue.
    //
    // ABI: PLUGIN_API_VERSION 1. Bottom-of-vtable append per the ABI rule
    // above — existing plugins compiled against version 0 keep working
    // because their vtables stop at getTransportSnapshot().
    virtual std::size_t drainSignalEmits(SignalEmitPod* out,
                                         std::size_t    max) noexcept = 0;
};

} // namespace entity::plugin
