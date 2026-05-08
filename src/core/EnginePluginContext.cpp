// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)

#include "entity/core/EnginePluginContext.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/core/Engine.hpp"
#include "entity/core/Settings.hpp"
#include "entity/plugin/Plugin.hpp"

#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>
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

bool EnginePluginContext::enqueueCommand(std::string_view typeName,
                                         std::string_view paramsJson) noexcept {
    if (m_engine == nullptr) {
        return false;
    }
    auto* dispatcher = m_engine->getCommandDispatcher();
    if (dispatcher == nullptr) {
        return false;
    }
    try {
        nlohmann::json params = paramsJson.empty()
                                    ? nlohmann::json::object()
                                    : nlohmann::json::parse(paramsJson);
        return dispatcher->enqueue(std::string(typeName), params);
    } catch (const std::exception& e) {
        std::cerr << "[plugin:" << m_pluginName
                  << "] enqueueCommand parse failed for " << typeName
                  << ": " << e.what() << '\n';
        return false;
    } catch (...) {
        return false;
    }
}

void EnginePluginContext::registerShutdownHook(
        entity::plugin::PluginShutdownFn hook) noexcept {
    if (m_engine && hook) {
        m_engine->addPluginShutdownHook(hook);
    }
}

bool EnginePluginContext::getBoolSetting(std::string_view key,
                                          bool defaultValue) const noexcept {
    const Settings s = activeSettings();
    if (key == "oscReceiverEnabled") return s.oscReceiverEnabled;
    return defaultValue;
}

int EnginePluginContext::getIntSetting(std::string_view key,
                                        int defaultValue) const noexcept {
    const Settings s = activeSettings();
    if (key == "oscReceiverPort") return static_cast<int>(s.oscReceiverPort);
    return defaultValue;
}

} // namespace entity::core
