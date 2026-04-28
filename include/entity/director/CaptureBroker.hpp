#pragma once

#include "entity/bus/Message.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace entity {

class CommandDispatcher;
namespace bus { class IMessageTransport; }

// Director-side broker for the asynchronous capture-command request/reply
// pattern (Phase D entry, subtask 7).
//
// `CaptureScreenshotCommand` and `CaptureHashCommand` no longer call
// IRenderer-touching code directly -- they enqueue a `RequestComposeCapture`
// on the bus and *park* the metadata for the script-result resolution
// (hash file, golden compare path, screenshot path, owning dispatcher)
// keyed by a monotonic correlation ID. The Renderer service drains the
// request during its `render()` tick, runs the existing capture pass, and
// publishes a `CaptureCompleted` reply on the R2D channel. Director's R2D
// drain (in `Engine::run()` after `render()`) routes the reply back here
// via `handleCaptureCompleted`, which finishes the resolution synchronously
// on the main thread: writes the hash file, performs the golden compare,
// and reports outcome through the dispatcher's script-results API.
//
// In-process today, sequential ticks. Thread-safety: the pending map is
// protected by an internal mutex so script-result resolution from any
// thread (today: just main) stays sound when Phase E swaps the transport
// for UDP and replies arrive on a transport-owned thread.
class CaptureBroker {
public:
    CaptureBroker() = default;
    ~CaptureBroker() = default;

    CaptureBroker(const CaptureBroker&) = delete;
    CaptureBroker& operator=(const CaptureBroker&) = delete;

    // Wired by Engine after Director and the transport are both alive. Both
    // pointers are non-owning; the broker outlives neither.
    void setTransport(bus::IMessageTransport* transport) { m_transport = transport; }
    void setCommandDispatcher(CommandDispatcher* dispatcher) { m_dispatcher = dispatcher; }

    // Returns true if the request was queued. Returns false (with an error
    // logged through the dispatcher) when the transport isn't wired yet --
    // shouldn't happen in normal flow, but the caller can decide whether to
    // fail the script or carry on.
    bool requestHashCapture(int slot,
                            std::string hashFilepath,
                            std::string goldenFilepath);
    bool requestScreenshotCapture(int slot,
                                  std::string pngFilepath,
                                  bool fullWindow);

    // Called from Engine::run()'s R2D drain. Looks up the parked entry and
    // applies the resolution logic (write hash file, golden compare, or
    // record the screenshot path on script results). Errors propagate
    // through the dispatcher's `addErrorToResults`.
    void handleCaptureCompleted(const bus::CaptureCompleted& reply);

    // True iff at least one parked request hasn't been resolved yet. Used
    // by the Engine run-loop to avoid exiting before pending captures
    // finish on Exit-then-quit scripts.
    bool hasPending() const;

private:
    enum class Kind {
        Hash,
        Screenshot
    };

    struct PendingCapture {
        Kind kind{Kind::Hash};
        std::string hashFilepath;
        std::string goldenFilepath;
        std::string screenshotFilepath;
    };

    bus::IMessageTransport* m_transport{nullptr};
    CommandDispatcher*      m_dispatcher{nullptr};

    mutable std::mutex m_mutex;
    std::uint64_t      m_nextCorrelationId{1};
    std::unordered_map<std::uint64_t, PendingCapture> m_pending;
};

} // namespace entity
