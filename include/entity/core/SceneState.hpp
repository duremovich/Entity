#pragma once

#include <entt/entt.hpp>

#include <atomic>
#include <cassert>

namespace entity {

// Single-process wrapper around entt::registry that polices Director/Renderer
// access discipline.
//
// Why this exists: Phase D feature work (timecode, OSC, audio, NDI) attaches
// naturally to either Director or Renderer, and Phase E lifts the two into
// separate processes. SceneState is the seam -- the implementation can grow
// into a registry replicated across the message bus without touching any
// caller. Today it's a thin wrapper.
//
// Existing call sites use registry() / cregistry() unguarded -- this subtask
// is a no-behavior-change addition. New code added in subtasks 4+ goes
// through ReadHandle / WriteHandle, which in debug builds assert no
// overlapping access mode (multiple concurrent ReadHandles OK; any
// WriteHandle is mutually exclusive with any reader or other writer).
//
// Sequential ticking on the main thread is the invariant: Director writes
// during its tick, Renderer reads during its tick. The asserts catch future
// thread-misuse cheaply; release builds compile them out and the handles
// become zero-overhead pass-throughs.
class SceneState {
public:
    explicit SceneState(entt::registry& registry) noexcept
        : m_registry(registry) {}

    SceneState(const SceneState&) = delete;
    SceneState& operator=(const SceneState&) = delete;
    SceneState(SceneState&&) = delete;
    SceneState& operator=(SceneState&&) = delete;

    // Direct access. Existing call sites (Engine, systems) use these without
    // guards; new code in later subtasks goes through read() / write().
    entt::registry& registry() noexcept { return m_registry; }
    const entt::registry& registry() const noexcept { return m_registry; }
    const entt::registry& cregistry() const noexcept { return m_registry; }

    class ReadHandle {
    public:
        explicit ReadHandle(SceneState& owner) noexcept;
        ~ReadHandle();
        ReadHandle(const ReadHandle&) = delete;
        ReadHandle& operator=(const ReadHandle&) = delete;
        ReadHandle(ReadHandle&&) = delete;
        ReadHandle& operator=(ReadHandle&&) = delete;
        const entt::registry& registry() const noexcept { return m_owner.m_registry; }
    private:
        SceneState& m_owner;
    };

    class WriteHandle {
    public:
        explicit WriteHandle(SceneState& owner) noexcept;
        ~WriteHandle();
        WriteHandle(const WriteHandle&) = delete;
        WriteHandle& operator=(const WriteHandle&) = delete;
        WriteHandle(WriteHandle&&) = delete;
        WriteHandle& operator=(WriteHandle&&) = delete;
        entt::registry& registry() noexcept { return m_owner.m_registry; }
    private:
        SceneState& m_owner;
    };

    ReadHandle read() noexcept { return ReadHandle(*this); }
    WriteHandle write() noexcept { return WriteHandle(*this); }

#ifndef NDEBUG
    // Diagnostics, debug only. Safe to call concurrently.
    int activeReaders() const noexcept { return m_readers.load(std::memory_order_acquire); }
    int activeWriters() const noexcept { return m_writers.load(std::memory_order_acquire); }
#endif

private:
    entt::registry& m_registry;

#ifndef NDEBUG
    std::atomic<int> m_readers{0};
    std::atomic<int> m_writers{0};
#endif

    friend class ReadHandle;
    friend class WriteHandle;
};

#ifndef NDEBUG

inline SceneState::ReadHandle::ReadHandle(SceneState& owner) noexcept
    : m_owner(owner) {
    assert(m_owner.m_writers.load(std::memory_order_acquire) == 0
        && "SceneState::ReadHandle: a WriteHandle is currently active");
    m_owner.m_readers.fetch_add(1, std::memory_order_acq_rel);
}

inline SceneState::ReadHandle::~ReadHandle() {
    m_owner.m_readers.fetch_sub(1, std::memory_order_acq_rel);
}

inline SceneState::WriteHandle::WriteHandle(SceneState& owner) noexcept
    : m_owner(owner) {
    assert(m_owner.m_readers.load(std::memory_order_acquire) == 0
        && "SceneState::WriteHandle: a ReadHandle is currently active");
    assert(m_owner.m_writers.load(std::memory_order_acquire) == 0
        && "SceneState::WriteHandle: another WriteHandle is currently active");
    m_owner.m_writers.fetch_add(1, std::memory_order_acq_rel);
}

inline SceneState::WriteHandle::~WriteHandle() {
    m_owner.m_writers.fetch_sub(1, std::memory_order_acq_rel);
}

#else

inline SceneState::ReadHandle::ReadHandle(SceneState& owner) noexcept : m_owner(owner) {}
inline SceneState::ReadHandle::~ReadHandle() = default;
inline SceneState::WriteHandle::WriteHandle(SceneState& owner) noexcept : m_owner(owner) {}
inline SceneState::WriteHandle::~WriteHandle() = default;

#endif

} // namespace entity
