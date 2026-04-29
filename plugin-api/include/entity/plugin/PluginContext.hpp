// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "Plugin.hpp"

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
};

} // namespace entity::plugin
