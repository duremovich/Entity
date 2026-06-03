// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)

#include "entity/core/EnginePluginContext.hpp"
#include "entity/command/CommandDispatcher.hpp"
#include "entity/core/Engine.hpp"
#include "entity/core/Settings.hpp"
#include "entity/plugin/Plugin.hpp"
#include "entity/project/ProjectManager.hpp"
#include "entity/timeline/Timeline.hpp"

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
    if (key == "oscSenderEnabled")   return s.oscSenderEnabled;
    if (key == "dmxArtnetEnabled")   return s.dmxArtnetEnabled;
    if (key == "dmxSacnEnabled")     return s.dmxSacnEnabled;
    if (key == "dmxOutEnabled")      return s.dmxOutEnabled;
    if (key == "dmxOutSacnEnabled")  return s.dmxOutSacnEnabled;
    if (key == "dmxEnttecEnabled")   return s.dmxEnttecEnabled;
    return defaultValue;
}

int EnginePluginContext::getIntSetting(std::string_view key,
                                        int defaultValue) const noexcept {
    const Settings s = activeSettings();
    if (key == "oscReceiverPort")    return static_cast<int>(s.oscReceiverPort);
    if (key == "dmxArtnetListenPort") return static_cast<int>(s.dmxArtnetListenPort);
    if (key == "dmxEnttecUniverse")  return static_cast<int>(s.dmxEnttecUniverse);
    return defaultValue;
}

std::string EnginePluginContext::getStringSetting(std::string_view key,
                                                   std::string_view defaultValue) const noexcept {
    const Settings s = activeSettings();
    if (key == "dmxOutArtnetTargets")        return s.dmxOutArtnetTargets;
    if (key == "dmxEnttecPort")              return s.dmxEnttecPort;
    if (key == "oscSenderDestinationsJson")  return s.oscSenderDestinationsJson;

    // Project-scoped mapping JSON tables. ProjectManager owns the active
    // project; we read its serialized tables through project-scoped keys.
    if (key == "dmxMappingsJson") {
        if (m_engine) {
            if (auto* pm = m_engine->getProjectManager()) {
                return pm->getDmxMappingsJson();
            }
        }
        return std::string(defaultValue);
    }
    if (key == "oscInboundMappingsJson") {
        if (m_engine) {
            if (auto* pm = m_engine->getProjectManager()) {
                return pm->getOscInboundMappingsJson();
            }
        }
        return std::string(defaultValue);
    }
    if (key == "oscOutboundMappingsJson") {
        if (m_engine) {
            if (auto* pm = m_engine->getProjectManager()) {
                return pm->getOscOutboundMappingsJson();
            }
        }
        return std::string(defaultValue);
    }
    return std::string(defaultValue);
}

std::size_t EnginePluginContext::drainSignalEmits(
        entity::plugin::SignalEmitPod* out, std::size_t max) noexcept {
    if (!out || max == 0) return 0;
    std::lock_guard<std::mutex> lk(m_signalMutex);
    std::size_t n = 0;
    while (n < max && !m_signalQueue.empty()) {
        out[n++] = m_signalQueue.front();
        m_signalQueue.pop();
    }
    // Reset the saturation flag once the queue is below the cap so the
    // next saturation episode logs exactly once again.
    if (m_signalQueue.size() < kMaxQueueDepth) {
        m_signalQueueSaturated = false;
    }
    return n;
}

void EnginePluginContext::postSignalEmit(
        const entity::plugin::SignalEmitPod& emit) noexcept {
    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lk(m_signalMutex);
        if (m_signalQueue.size() >= kMaxQueueDepth) {
            // Log at most once per saturation episode; decision made under lock,
            // actual I/O happens outside (so the drain path never blocks on cout).
            if (!m_signalQueueSaturated) {
                m_signalQueueSaturated = true;
                shouldLog = true;
            }
            return;
        }
        m_signalQueue.push(emit);
    }
    if (shouldLog) {
        log(entity::plugin::LogLevel::Warn,
            "EnginePluginContext: signal emit queue full (256), dropping");
    }
}

entity::plugin::TransportSnapshot
EnginePluginContext::getTransportSnapshot() const noexcept {
    using entity::plugin::TransportSnapshot;
    using entity::plugin::TransportState;

    TransportSnapshot snap;
    if (!m_engine) return snap;

    auto* tl = m_engine->getTimeline();
    if (!tl) return snap;

    // Playback state — both fields are atomic; safe cross-thread.
    const auto ps = tl->getPlaybackState();
    if (ps == entity::PlaybackState::Playing)
        snap.playbackState = TransportState::Playing;
    else if (ps == entity::PlaybackState::Paused)
        snap.playbackState = TransportState::Paused;
    else
        snap.playbackState = TransportState::Stopped;

    const auto currentFrame = tl->getCurrentFrame();
    snap.frameNumber = static_cast<int64_t>(currentFrame);

    // Section state + frame rate — uses shared lock internally so this call
    // is safe from the OSC sender worker thread concurrently with editor-thread
    // addSectionBreak / removeSectionBreak / setFrameRate mutations.
    tl->snapshotSectionsAndRate(currentFrame,
                                snap.frameRate,
                                snap.activeSectionIndex,
                                snap.activeSectionFrame,
                                snap.nextSectionIndex,
                                snap.nextSectionFrame);

    // Project name — thread-safe via ProjectManager's shared lock.
    if (auto* pm = m_engine->getProjectManager()) {
        snap.projectName = pm->getProjectName();
    }

    return snap;
}

} // namespace entity::core
