#pragma once

#include <atomic>
#include <cstdint>

namespace entity {

// Editor-frame progress counters + a per-resource generation gate (#75).
// Together they let the show thread mutate GPU state the editor references
// (descriptor rewrites, resource swaps) without a lock, by deferring the
// mutation until no editor command list — recorded or submitted — can still
// reference the old state.
//
// Protocol:
//   Editor thread:  tracker.beginFrame()
//                   ... record; readers call gate.isReadable() before
//                       grabbing GPU handles (closed gate = "skip this
//                       resource this frame") ...
//                   ExecuteCommandLists → tracker.endFrame()
//   Show thread:    gate.close(tracker)           — readers start skipping
//                   if (!gate.mayMutate(tracker))  — defer, retry next tick
//                   waitForGpu()                   — drain submitted work
//                   <mutate resources/descriptors>
//                   gate.open()                    — readers resume
//
// Why this is sufficient: every editor frame either (a) began before
// close(), in which case mayMutate() stays false until that frame's command
// list is submitted (and the GPU drain then covers it), or (b) began after
// close(), in which case its isReadable() check observes the closed gate and
// the frame never touches the resource. So at mutation time no editor
// command list anywhere — mid-record, queued, or executing — references the
// old state. Reads that don't grab GPU handles (width/height) are covered
// too, provided the frame checked isReadable()/a gated accessor first.
//
// The seq_cst everywhere is load-bearing. The (b) case needs a single total
// order across two atomics (generation, framesBegun): if close()'s
// framesBegun snapshot missed a frame, that frame's begin-increment is later
// in the seq_cst total order, so its subsequent generation load must observe
// the closed gate. Plain acquire/release gives no such cross-variable
// guarantee. On x86 the cost is one locked RMW per editor frame — noise.
//
// Not to be confused with EditorFrameRing (EditorFrameRing.hpp): that is
// single-threaded fence-ring bookkeeping for allocator/ImGui-buffer reuse.
// This tracker counts the same begin/end events but exists for cross-thread
// mutation gating and deliberately counts device-lost/failed frames too —
// the two cannot be merged (see the m_editorFrameCounter comment in
// D3D12Renderer.hpp).

class EditorFrameTracker {
public:
    // Editor thread, first thing in beginEditorFrame().
    void beginFrame() { m_begun.fetch_add(1, std::memory_order_seq_cst); }

    // Editor thread, once the frame's command list has been submitted (or
    // will never be — failure paths must call this too, or gates starve).
    void endFrame() { m_submitted.fetch_add(1, std::memory_order_seq_cst); }

    std::uint64_t framesBegun() const {
        return m_begun.load(std::memory_order_seq_cst);
    }
    std::uint64_t framesSubmitted() const {
        return m_submitted.load(std::memory_order_seq_cst);
    }

private:
    std::atomic<std::uint64_t> m_begun{0};
    std::atomic<std::uint64_t> m_submitted{0};
};

class EditorReadGate {
public:
    EditorReadGate() = default;
    // std::vector requires MoveInsertable; only ever exercised while the
    // reader thread cannot observe the element (see ComposeTarget).
    EditorReadGate(EditorReadGate&& o) noexcept
        : m_generation(o.m_generation.load(std::memory_order_relaxed))
        , m_gateFrame(o.m_gateFrame) {}
    EditorReadGate& operator=(EditorReadGate&&) = delete;
    EditorReadGate(const EditorReadGate&) = delete;
    EditorReadGate& operator=(const EditorReadGate&) = delete;

    // Reader (editor thread). False = a mutation is pending or in progress:
    // skip this resource for the rest of the frame. Must be checked before
    // any other read of the gated resource in the frame.
    bool isReadable() const {
        return (m_generation.load(std::memory_order_seq_cst) & 1u) == 0u;
    }

    // --- Writer side. All calls below must come from the one writer thread.

    bool isClosed() const {
        return (m_generation.load(std::memory_order_relaxed) & 1u) != 0u;
    }

    // Close the gate (idempotent). Records the editor-frame high-water mark;
    // frames begun before it may still hold handles, so mayMutate() waits
    // for them to submit. Re-closing keeps the original mark — readers have
    // been gated since then.
    void close(const EditorFrameTracker& tracker) {
        if (!isClosed()) {
            m_generation.fetch_add(1, std::memory_order_seq_cst);
            m_gateFrame = tracker.framesBegun();
        }
    }

    // True once every editor frame begun before close() has submitted its
    // command list. Combined with a GPU drain, the writer may then mutate.
    bool mayMutate(const EditorFrameTracker& tracker) const {
        return isClosed() && tracker.framesSubmitted() >= m_gateFrame;
    }

    // Reopen the gate: publish after a mutation, or cancel a pending close
    // that turned out to be unnecessary. Idempotent — a no-op on an open
    // gate (an unconditional increment would flip the gate closed and
    // strand readers).
    void open() {
        if (isClosed()) {
            m_generation.fetch_add(1, std::memory_order_seq_cst);
        }
    }

private:
    std::atomic<std::uint32_t> m_generation{0}; // even = readable, odd = closed
    std::uint64_t m_gateFrame{0};               // writer-thread bookkeeping
};

} // namespace entity
