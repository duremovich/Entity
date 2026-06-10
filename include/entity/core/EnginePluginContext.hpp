// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
#pragma once

#include "entity/plugin/PluginContext.hpp"

#include <mutex>
#include <queue>
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
    std::string                      getStringSetting(std::string_view key,
                                                      std::string_view defaultValue) const noexcept override;
    entity::plugin::TransportSnapshot getTransportSnapshot() const noexcept override;

    // IPluginContext v1: drain accumulated SignalEmit events. Thread-safe.
    std::size_t drainSignalEmits(entity::plugin::SignalEmitPod* out,
                                  std::size_t max) noexcept override;

    // IPluginContext v2: remote-control live plane (ADR-0028).
    bool setRemoteParam(std::string_view patchId,
                        std::string_view param,
                        double value) noexcept override;
    bool setRemoteParamString(std::string_view patchId,
                              std::string_view param,
                              std::string_view value) noexcept override;

    // Called by the show thread (SignalOutputSystem, Phase 5/6) or by test
    // script commands (Phase 2 verification) to enqueue a signal for plugin
    // consumption. Thread-safe.
    void postSignalEmit(const entity::plugin::SignalEmitPod& emit) noexcept;

private:
    Engine*     m_engine;
    std::string m_pluginName;

    // Pending signal emits queued for plugin drain. Written by the show thread
    // (or script command dispatch), read by any plugin worker that calls
    // drainSignalEmits(). Capacity-capped at 256 to bound memory under a
    // misbehaving producer; excess events are dropped with a log.
    //
    // m_signalQueueSaturated: set true (under m_signalMutex) on the first drop
    // event per saturation episode; reset to false in drainSignalEmits when
    // the queue empties back below kMaxQueueDepth. The log call happens OUTSIDE
    // the lock so the osc-sender drain path is never blocked by console I/O.
    static constexpr std::size_t kMaxQueueDepth = 256;
    mutable std::mutex                              m_signalMutex;
    std::queue<entity::plugin::SignalEmitPod>       m_signalQueue;
    bool                                            m_signalQueueSaturated{false};
};

} // namespace entity::core
