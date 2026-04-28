#include "entity/director/CaptureBroker.hpp"

#include "entity/bus/IMessageTransport.hpp"
#include "entity/bus/Serialization.hpp"
#include "entity/command/CommandDispatcher.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <utility>

namespace entity {

namespace {

void publishRequest(bus::IMessageTransport& transport,
                    const bus::RequestComposeCapture& msg) {
    transport.send(bus::Direction::D2R,
                   bus::serialize(bus::Message{msg}));
}

} // anonymous namespace

bool CaptureBroker::requestHashCapture(int slot,
                                       std::string hashFilepath,
                                       std::string goldenFilepath) {
    if (!m_transport) {
        if (m_dispatcher) {
            m_dispatcher->addErrorToResults(
                "[CaptureBroker] Hash request dropped: transport not wired");
        }
        return false;
    }

    bus::RequestComposeCapture msg{};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        msg.correlationId = m_nextCorrelationId++;
        msg.slot = slot;
        msg.hashOnly = true;
        msg.fullWindow = false;
        msg.goldenHashPath = goldenFilepath;
        m_pending.emplace(msg.correlationId,
                          PendingCapture{Kind::Hash,
                                         std::move(hashFilepath),
                                         std::move(goldenFilepath),
                                         {}});
    }
    publishRequest(*m_transport, msg);
    return true;
}

bool CaptureBroker::requestScreenshotCapture(int slot,
                                             std::string pngFilepath,
                                             bool fullWindow) {
    if (!m_transport) {
        if (m_dispatcher) {
            m_dispatcher->addErrorToResults(
                "[CaptureBroker] Screenshot request dropped: transport not wired");
        }
        return false;
    }

    bus::RequestComposeCapture msg{};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        msg.correlationId = m_nextCorrelationId++;
        msg.slot = slot;
        msg.hashOnly = false;
        msg.fullWindow = fullWindow;
        msg.pngPath = pngFilepath;
        m_pending.emplace(msg.correlationId,
                          PendingCapture{Kind::Screenshot,
                                         {},
                                         {},
                                         std::move(pngFilepath)});
    }
    publishRequest(*m_transport, msg);
    return true;
}

void CaptureBroker::handleCaptureCompleted(const bus::CaptureCompleted& reply) {
    PendingCapture entry{};
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_pending.find(reply.correlationId);
        if (it == m_pending.end()) {
            std::cerr << "[CaptureBroker] Stray CaptureCompleted id="
                      << reply.correlationId << " -- no parked request" << std::endl;
            return;
        }
        entry = std::move(it->second);
        m_pending.erase(it);
    }

    if (!reply.ok) {
        if (m_dispatcher) {
            m_dispatcher->addErrorToResults(
                std::string("[CaptureBroker] Capture failed: ") + reply.errorMessage);
        }
        return;
    }

    if (entry.kind == Kind::Screenshot) {
        if (m_dispatcher) {
            m_dispatcher->addScreenshotToResults(entry.screenshotFilepath);
        }
        return;
    }

    // Hash path: format the line, write the hash file, compare to the
    // golden if one was supplied. This is the same line format Engine's
    // pre-bus captureHash() produced -- "<16-hex> <WxH>\n". The Renderer
    // sends back hexHash plus the dimensions encoded in the same string.
    // We don't have width/height here; the Renderer formats the line for
    // us in `reply.hexHash`.
    if (entry.hashFilepath.empty()) {
        if (m_dispatcher) {
            m_dispatcher->addErrorToResults(
                "[CaptureBroker] Hash request resolved without a target file");
        }
        return;
    }

    std::filesystem::path outPath(entry.hashFilepath);
    if (outPath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(outPath.parent_path(), ec);
    }

    std::ofstream out(outPath, std::ios::binary);
    if (!out) {
        if (m_dispatcher) {
            m_dispatcher->addErrorToResults(
                std::string("[CaptureBroker] failed to open hash file: ") +
                entry.hashFilepath);
        }
        return;
    }
    out.write(reply.hexHash.data(),
              static_cast<std::streamsize>(reply.hexHash.size()));
    out.close();

    std::cout << "[CaptureHash] " << entry.hashFilepath << " -> " << reply.hexHash;

    if (entry.goldenFilepath.empty()) {
        return;
    }

    std::ifstream gf(entry.goldenFilepath, std::ios::binary);
    if (!gf) {
        std::cerr << "[CaptureHash] MISS: golden not found: "
                  << entry.goldenFilepath << std::endl;
        if (m_dispatcher) {
            m_dispatcher->addErrorToResults(
                std::string("Hash golden missing: ") + entry.goldenFilepath);
        }
        return;
    }
    std::string golden((std::istreambuf_iterator<char>(gf)),
                        std::istreambuf_iterator<char>());
    if (golden != reply.hexHash) {
        std::ostringstream oss;
        oss << "[CaptureHash] FAIL mismatch\n  got:    " << reply.hexHash
            << "  golden: " << golden;
        std::cerr << oss.str() << std::endl;
        if (m_dispatcher) {
            m_dispatcher->addErrorToResults(oss.str());
        }
        return;
    }
    std::cout << "[CaptureHash] PASS against " << entry.goldenFilepath << std::endl;
}

bool CaptureBroker::hasPending() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_pending.empty();
}

} // namespace entity
