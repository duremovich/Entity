#include "entity/input/InputBus.hpp"

namespace entity {

void InputBus::setFloat(std::string_view name, float v) {
    std::shared_ptr<Channel> ch;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string key{name};
        auto it = m_channels.find(key);
        if (it == m_channels.end()) {
            ch = std::make_shared<Channel>();
            m_channels.emplace(std::move(key), ch);
        } else {
            ch = it->second;
        }
    }
    ch->value.store(v, std::memory_order_relaxed);
}

float InputBus::getFloat(std::string_view name, float fallback) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_channels.find(std::string{name});
    if (it == m_channels.end()) return fallback;
    return it->second->value.load(std::memory_order_relaxed);
}

} // namespace entity
