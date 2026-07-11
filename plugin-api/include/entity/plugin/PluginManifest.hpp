// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich
#pragma once

#include "Plugin.hpp"

#include <string>

namespace entity::plugin {

// In-memory mirror of a plugin's manifest.json. Each plugin ships a
// manifest.json next to its CMakeLists.txt; the build system copies it to
// the install tree and the engine validates it at load time.
//
// Wire format (manifest.json) -- example:
//
// {
//   "name":               "bus-logger",
//   "version":            "0.1.0",
//   "license":            "MIT",
//   "requiredApiVersion": 2,
//   "description":        "Logs bus traffic to stdout for debugging.",
//   "kind":               "control-plane"
// }
//
// `requiredApiVersion` is the MINIMUM engine API the plugin can run
// against: declare the highest PLUGIN_API_VERSION whose IPluginContext
// methods your plugin actually calls (each versioned method family is
// tagged "ABI: PLUGIN_API_VERSION n" in PluginContext.hpp; untagged
// methods are version 0). Declaring lower than that lets an older engine
// load the plugin and dispatch calls past the end of its vtable --
// undefined behavior. When in doubt, declare the current
// PLUGIN_API_VERSION (the struct's default below).
//
// `kind` is informational today (drives the static dispatcher's link
// rules in CMake -- "hot-path" plugins skip the entity-bus link). When
// dynamic loading lands it'll gate which loader path applies.
struct PluginManifest {
    std::string name;
    std::string version;
    std::string license;
    int         requiredApiVersion{PLUGIN_API_VERSION};
    std::string description;
    std::string kind; // "control-plane" or "hot-path"
};

} // namespace entity::plugin
