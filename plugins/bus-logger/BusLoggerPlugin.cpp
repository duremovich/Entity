// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Dylan Uremovich

#include "entity/plugin/PluginContext.hpp"

#include <string>

// Reference plugin. The minimum a plugin must do to prove the pipeline
// works: validate the API version, log a registration message, return 0.
//
// A real plugin would also subscribe to bus messages here, install a
// shutdown hook, or stash `ctx` for later use. See plugins/README.md for
// the full pattern.
extern "C" int entity_plugin_register_bus_logger(entity::plugin::IPluginContext* ctx) {
    if (ctx == nullptr) {
        return -1;
    }

    if (ctx->apiVersion() != entity::plugin::PLUGIN_API_VERSION) {
        ctx->log(entity::plugin::LogLevel::Error,
                 std::string("API version mismatch: engine=") +
                 std::to_string(ctx->apiVersion()) +
                 " plugin=" +
                 std::to_string(entity::plugin::PLUGIN_API_VERSION));
        return -2;
    }

    ctx->log(entity::plugin::LogLevel::Info, "bus-logger registered");
    return 0;
}
