// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
#pragma once

#include "entity/plugin/PluginContext.hpp"

#include <string>

namespace entity {
class Engine;
} // namespace entity

namespace entity::core {

// Engine-side implementation of `entity::plugin::IPluginContext`. Owns no
// state of its own beyond a back-pointer to the Engine and a configurable
// plugin-name used as a log prefix. One instance is created in main.cpp
// after `Engine::initialize()` and passed to `registerStaticPlugins()`.
//
// Lives in the GPL core (linking against the Apache 2.0 plugin-api
// headers, which the linking exception in LICENSE explicitly
// permits).
class EnginePluginContext final : public entity::plugin::IPluginContext {
public:
    EnginePluginContext(Engine* engine, std::string pluginName);
    ~EnginePluginContext() override = default;

    EnginePluginContext(const EnginePluginContext&) = delete;
    EnginePluginContext& operator=(const EnginePluginContext&) = delete;

    int                              apiVersion()  const noexcept override;
    entity::bus::IMessageTransport*  bus()               noexcept override;
    void                             log(entity::plugin::LogLevel level,
                                         std::string_view message) noexcept override;
    std::string_view                 pluginName()  const noexcept override;
    bool                             enqueueCommand(std::string_view typeName,
                                                    std::string_view paramsJson) noexcept override;
    void                             registerShutdownHook(
                                         entity::plugin::PluginShutdownFn hook) noexcept override;
    bool                             getBoolSetting(std::string_view key,
                                                    bool defaultValue) const noexcept override;
    int                              getIntSetting(std::string_view key,
                                                   int defaultValue) const noexcept override;

private:
    Engine*     m_engine;
    std::string m_pluginName;
};

} // namespace entity::core
