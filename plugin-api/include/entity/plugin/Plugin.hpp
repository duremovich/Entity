// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

// Entry-point declarations for Entity plugins.
//
// A plugin (static or dynamic) exposes one C-linkage function:
//
//     extern "C" int entity_plugin_register_<name>(IPluginContext* ctx);
//
// The function returns 0 on success, a negative value on failure. The plugin
// may register handlers, subscribe to bus messages, or stash the context for
// later use. Symmetric shutdown is optional; if a plugin needs to release
// resources, it can install a callback during register and let the engine
// invoke it during shutdown.
//
// For static-linked plugins, the function name is supplied to the build
// system via `entity_register_plugin()` in cmake/EntityPlugins.cmake; the
// generated dispatcher (StaticRegistry.cpp) calls every registered function
// at engine startup.
//
// For dynamic-loaded plugins (future), the loader will resolve the symbol
// via dlsym/GetProcAddress against a fixed name pattern. The convention --
// `entity_plugin_register_<name>` -- carries forward unchanged.

#include <cstdint>

namespace entity::plugin {

// Bumped on any incompatible change to plugin-api/. A plugin whose manifest
// declares a requiredApiVersion newer than the engine's PLUGIN_API_VERSION
// is refused at load time (the engine's vtable would be missing methods the
// plugin calls). Version history: 1 added drainSignalEmits (ADR-0027);
// 2 added setRemoteParam/setRemoteParamString (remote-control plane).
constexpr int PLUGIN_API_VERSION = 2;

class IPluginContext;

// Required entry point: returns 0 on success, < 0 on failure.
using PluginRegisterFn = int (*)(IPluginContext*);

// Optional shutdown hook a plugin may install during register().
using PluginShutdownFn = void (*)();

// Defined by the generated StaticRegistry.cpp (linked into the editor
// executable). Calls every static-linked plugin's register function in
// CMake registration order. Call once at engine startup, after the bus
// transport is constructed.
//
// The runtime-loaded counterpart (future) will live in
// `loadDynamicPlugins(IPluginContext*, const std::filesystem::path&)`.
void registerStaticPlugins(IPluginContext* ctx);

} // namespace entity::plugin
