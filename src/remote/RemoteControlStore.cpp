#include "entity/remote/RemoteControlStore.hpp"

namespace entity::remote {

int RemoteControlStore::findSlotLocked(std::string_view patchId) const {
    auto it = m_idToSlot.find(std::string(patchId));
    return (it == m_idToSlot.end()) ? -1 : it->second;
}

int RemoteControlStore::allocateSlot(const std::string& patchId) {
    if (patchId.empty()) return -1;
    std::lock_guard<std::mutex> lock(m_idMutex);
    if (m_idToSlot.count(patchId)) return -1;
    for (int i = 0; i < kMaxRemotePatches; ++i) {
        Slot& s = m_slots[static_cast<std::size_t>(i)];
        if (s.inUse.load(std::memory_order_relaxed)) continue;
        for (auto& v : s.values) v.store(0.0f, std::memory_order_relaxed);
        s.presentMask.store(0, std::memory_order_relaxed);
        s.engagedFlag.store(0, std::memory_order_relaxed);
        s.textGen.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> tl(s.textMutex);
            s.text.clear();
        }
        s.inUse.store(true, std::memory_order_release);
        m_idToSlot.emplace(patchId, i);
        return i;
    }
    return -1;  // table full
}

void RemoteControlStore::freeSlot(int slot) {
    if (slot < 0 || slot >= kMaxRemotePatches) return;
    std::lock_guard<std::mutex> lock(m_idMutex);
    for (auto it = m_idToSlot.begin(); it != m_idToSlot.end(); ++it) {
        if (it->second == slot) { m_idToSlot.erase(it); break; }
    }
    Slot& s = m_slots[static_cast<std::size_t>(slot)];
    s.engagedFlag.store(0, std::memory_order_relaxed);
    s.presentMask.store(0, std::memory_order_relaxed);
    s.inUse.store(false, std::memory_order_release);
}

bool RemoteControlStore::renameSlot(int slot, const std::string& newId) {
    if (slot < 0 || slot >= kMaxRemotePatches || newId.empty()) return false;
    std::lock_guard<std::mutex> lock(m_idMutex);
    if (m_idToSlot.count(newId)) return false;
    for (auto it = m_idToSlot.begin(); it != m_idToSlot.end(); ++it) {
        if (it->second == slot) {
            m_idToSlot.erase(it);
            m_idToSlot.emplace(newId, slot);
            return true;
        }
    }
    return false;
}

void RemoteControlStore::reset() {
    std::lock_guard<std::mutex> lock(m_idMutex);
    m_idToSlot.clear();
    for (auto& s : m_slots) {
        s.engagedFlag.store(0, std::memory_order_relaxed);
        s.presentMask.store(0, std::memory_order_relaxed);
        s.inUse.store(false, std::memory_order_release);
    }
}

void RemoteControlStore::setEngaged(int slot, bool e) {
    if (slot < 0 || slot >= kMaxRemotePatches) return;
    m_slots[static_cast<std::size_t>(slot)]
        .engagedFlag.store(e ? 1u : 0u, std::memory_order_release);
}

bool RemoteControlStore::setParamById(std::string_view patchId,
                                      RemoteParam p, float value) {
    std::lock_guard<std::mutex> lock(m_idMutex);
    const int slot = findSlotLocked(patchId);
    if (slot < 0) return false;
    Slot& s = m_slots[static_cast<std::size_t>(slot)];
    s.values[static_cast<std::size_t>(p)].store(value,
                                                std::memory_order_relaxed);
    s.presentMask.fetch_or(1u << static_cast<unsigned>(p),
                           std::memory_order_release);
    return true;
}

bool RemoteControlStore::setEngagedById(std::string_view patchId, bool e) {
    std::lock_guard<std::mutex> lock(m_idMutex);
    const int slot = findSlotLocked(patchId);
    if (slot < 0) return false;
    m_slots[static_cast<std::size_t>(slot)]
        .engagedFlag.store(e ? 1u : 0u, std::memory_order_release);
    return true;
}

bool RemoteControlStore::setTextById(std::string_view patchId,
                                     std::string_view text) {
    std::lock_guard<std::mutex> lock(m_idMutex);
    const int slot = findSlotLocked(patchId);
    if (slot < 0) return false;
    Slot& s = m_slots[static_cast<std::size_t>(slot)];
    {
        std::lock_guard<std::mutex> tl(s.textMutex);
        s.text.assign(text);
    }
    s.textGen.fetch_add(1, std::memory_order_release);
    return true;
}

RemoteControlStore::Sample RemoteControlStore::sample(int slot) const noexcept {
    Sample out;
    if (slot < 0 || slot >= kMaxRemotePatches) return out;
    const Slot& s = m_slots[static_cast<std::size_t>(slot)];
    if (!s.inUse.load(std::memory_order_acquire)) return out;
    out.engaged     = s.engagedFlag.load(std::memory_order_acquire) != 0;
    out.presentMask = s.presentMask.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < kRemoteParamCount; ++i) {
        out.values[i] = s.values[i].load(std::memory_order_relaxed);
    }
    return out;
}

bool RemoteControlStore::engaged(int slot) const noexcept {
    if (slot < 0 || slot >= kMaxRemotePatches) return false;
    return m_slots[static_cast<std::size_t>(slot)]
               .engagedFlag.load(std::memory_order_acquire) != 0;
}

std::optional<std::string>
RemoteControlStore::consumeText(int slot, std::uint32_t& lastSeenGen) const {
    if (slot < 0 || slot >= kMaxRemotePatches) return std::nullopt;
    const Slot& s = m_slots[static_cast<std::size_t>(slot)];
    const std::uint32_t gen = s.textGen.load(std::memory_order_acquire);
    if (gen == lastSeenGen) return std::nullopt;
    lastSeenGen = gen;
    std::lock_guard<std::mutex> tl(s.textMutex);
    return s.text;
}

} // namespace entity::remote
