// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "Plugin.hpp"

#include <string>
#include <string_view>

// Forward declaration -- plugins that touch the bus link entity-bus
// separately and include the full header. Plugins that don't touch the bus
// pay nothing for this declaration.
namespace entity::bus {
class IMessageTransport;
}

namespace entity::plugin {

enum class LogLevel : int {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
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
// Boundary rule: every method here is noexcept, returns trivially-copyable
// data or stable pointers, and never exposes core internals. New methods
// land at the bottom of the vtable so existing compiled plugins keep
// working across patch-version bumps to the engine.
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
    // the same accessor — Phase 5 special-cases certain keys to read
    // from the active project rather than the Settings struct, e.g.
    // "dmxMappingsJson" returns the active project's JSON-serialized
    // DMX mapping table).
    //
    // Same ABI rule: bottom of vtable, no PLUGIN_API_VERSION bump.
    // The signature returns by value to keep the boundary copy-safe
    // (no caller-managed buffer lifetime).
    virtual std::string getStringSetting(std::string_view key,
                                         std::string_view defaultValue) const noexcept = 0;
};

} // namespace entity::plugin
