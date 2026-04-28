#include "entity/bus/InMemoryMessageTransport.hpp"

#include <utility>

namespace entity::bus {

InMemoryMessageTransport::Channel& InMemoryMessageTransport::channel(Direction d) {
    return d == Direction::D2R ? m_d2r : m_r2d;
}

const InMemoryMessageTransport::Channel& InMemoryMessageTransport::channel(Direction d) const {
    return d == Direction::D2R ? m_d2r : m_r2d;
}

void InMemoryMessageTransport::send(Direction d, std::vector<std::uint8_t> bytes) {
    auto& c = channel(d);
    std::lock_guard<std::mutex> lk(c.mu);
    c.q.push(std::move(bytes));
}

void InMemoryMessageTransport::drain(Direction d, const Sink& sink) {
    auto& c = channel(d);

    // Steal the queue under the lock, deliver after release. Sinks may
    // re-enter send() on the same direction (replies, fan-outs); holding
    // the mutex across user code would self-deadlock and would also stall
    // unrelated producers.
    std::queue<std::vector<std::uint8_t>> local;
    {
        std::lock_guard<std::mutex> lk(c.mu);
        std::swap(local, c.q);
    }

    while (!local.empty()) {
        sink(std::move(local.front()));
        local.pop();
    }
}

std::size_t InMemoryMessageTransport::pending(Direction d) const {
    const auto& c = channel(d);
    std::lock_guard<std::mutex> lk(c.mu);
    return c.q.size();
}

} // namespace entity::bus
