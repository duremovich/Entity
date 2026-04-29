// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)

#include "entity/core/EnginePluginContext.hpp"
#include "entity/core/Engine.hpp"
#include "entity/plugin/Plugin.hpp"

#include <iostream>
#include <utility>

namespace entity::core {

EnginePluginContext::EnginePluginContext(Engine* engine, std::string pluginName)
    : m_engine(engine)
    , m_pluginName(std::move(pluginName)) {}

int EnginePluginContext::apiVersion() const noexcept {
    return entity::plugin::PLUGIN_API_VERSION;
}

entity::bus::IMessageTransport* EnginePluginContext::bus() noexcept {
    return m_engine ? m_engine->getBusTransport() : nullptr;
}

void EnginePluginContext::log(entity::plugin::LogLevel level,
                              std::string_view message) noexcept {
    const char* levelTag = "INFO";
    switch (level) {
        case entity::plugin::LogLevel::Debug: levelTag = "DEBUG"; break;
        case entity::plugin::LogLevel::Info:  levelTag = "INFO";  break;
        case entity::plugin::LogLevel::Warn:  levelTag = "WARN";  break;
        case entity::plugin::LogLevel::Error: levelTag = "ERROR"; break;
    }
    std::ostream& out = (level >= entity::plugin::LogLevel::Warn)
                           ? std::cerr
                           : std::cout;
    out << "[plugin:" << m_pluginName << "] [" << levelTag << "] "
        << message << '\n';
}

std::string_view EnginePluginContext::pluginName() const noexcept {
    return m_pluginName;
}

} // namespace entity::core
