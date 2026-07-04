/**
 * Unit tests for EditorFrameTracker + EditorReadGate (#75).
 *
 * The pair lets the show thread swap compose-target resources and rewrite
 * their descriptors without a lock: the gate closes (editor readers skip the
 * slot), mutation is deferred until every editor frame begun before the
 * close has submitted its command list, then the gate reopens.
 *
 * Deterministic tests cover the protocol state machine; the stress test
 * runs a fake editor-frame loop against a writer that mutates a multi-word
 * payload only when the protocol says it may, and asserts no frame that was
 * allowed to read ever observes a torn payload. A broken protocol (e.g.
 * mayMutate ignoring in-flight frames, or relaxed ordering on the
 * generation) makes the stress test fail readily under load.
 */

#include <gtest/gtest.h>

#include "entity/render/EditorReadGate.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

using namespace entity;

namespace {

TEST(EditorReadGate, OpensReadableAndIdle) {
    EditorReadGate gate;
    EXPECT_TRUE(gate.isReadable());
    EXPECT_FALSE(gate.isClosed());
}

TEST(EditorReadGate, CloseBlocksReadersUntilOpen) {
    EditorFrameTracker tracker;
    EditorReadGate gate;

    gate.close(tracker);
    EXPECT_FALSE(gate.isReadable());
    EXPECT_TRUE(gate.isClosed());

    gate.open();
    EXPECT_TRUE(gate.isReadable());
    EXPECT_FALSE(gate.isClosed());
}

TEST(EditorReadGate, MayMutateWaitsForMidRecordFrame) {
    EditorFrameTracker tracker;
    EditorReadGate gate;

    tracker.beginFrame();          // editor frame mid-record
    gate.close(tracker);
    EXPECT_FALSE(gate.mayMutate(tracker));  // frame may hold old handles

    tracker.endFrame();            // frame submitted
    EXPECT_TRUE(gate.mayMutate(tracker));
}

TEST(EditorReadGate, MayMutateImmediateWhenNoFrameInFlight) {
    EditorFrameTracker tracker;
    EditorReadGate gate;

    tracker.beginFrame();
    tracker.endFrame();
    gate.close(tracker);
    EXPECT_TRUE(gate.mayMutate(tracker));
}

TEST(EditorReadGate, FramesBegunAfterCloseDoNotDelayMutation) {
    EditorFrameTracker tracker;
    EditorReadGate gate;

    gate.close(tracker);
    // Frames begun after the close observe the closed gate and never touch
    // the resource, so they must not hold the mutation hostage.
    tracker.beginFrame();
    EXPECT_TRUE(gate.mayMutate(tracker));
    tracker.endFrame();
}

TEST(EditorReadGate, ReCloseKeepsOriginalHighWaterMark) {
    EditorFrameTracker tracker;
    EditorReadGate gate;

    tracker.beginFrame();
    gate.close(tracker);           // mark = 1 (frame 1 mid-record)
    tracker.endFrame();

    tracker.beginFrame();          // frame 2 begins post-close: gated
    gate.close(tracker);           // idempotent; mark must stay at 1
    EXPECT_TRUE(gate.mayMutate(tracker));
    tracker.endFrame();
}

TEST(EditorReadGate, MayMutateFalseWhileOpen) {
    EditorFrameTracker tracker;
    EditorReadGate gate;
    EXPECT_FALSE(gate.mayMutate(tracker));  // never mutate through an open gate
}

TEST(EditorReadGate, OpenIsIdempotentOnOpenGate) {
    EditorFrameTracker tracker;
    EditorReadGate gate;

    // The endShowFrame liveness backstop calls open() unconditionally each
    // tick; on an already-open gate it must be a no-op, not a re-close.
    gate.open();
    EXPECT_TRUE(gate.isReadable());
    gate.open();
    EXPECT_TRUE(gate.isReadable());

    // And a later close/open cycle still behaves.
    gate.close(tracker);
    EXPECT_FALSE(gate.isReadable());
    gate.open();
    EXPECT_TRUE(gate.isReadable());
}

TEST(EditorReadGate, CancelViaOpenRestoresReaders) {
    EditorFrameTracker tracker;
    EditorReadGate gate;

    tracker.beginFrame();
    gate.close(tracker);
    EXPECT_FALSE(gate.mayMutate(tracker));
    gate.open();                   // deferred resize became unnecessary
    EXPECT_TRUE(gate.isReadable());
    tracker.endFrame();
}

// Stress: fake editor thread runs begin/read/end frames while the writer
// thread repeatedly closes the gate, waits for mayMutate, mutates a payload
// word-by-word (with deliberate scheduling points to widen the window), and
// reopens. The reader checks isReadable() once per frame — mirroring how
// StageWindow gates on isComposeTargetReady()/getComposeTargetTextureID() —
// and if allowed, re-reads the payload throughout the frame expecting it
// to be internally consistent every time.
TEST(EditorReadGate, StressNoTornReadsUnderConcurrentMutation) {
    constexpr int      PAYLOAD_WORDS = 8;
    constexpr int      MUTATIONS     = 4000;

    EditorFrameTracker tracker;
    EditorReadGate     gate;
    uint64_t           payload[PAYLOAD_WORDS] = {};

    std::atomic<bool>  writerDone{false};
    std::atomic<bool>  torn{false};

    std::thread editor([&] {
        while (!writerDone.load(std::memory_order_relaxed) &&
               !torn.load(std::memory_order_relaxed)) {
            tracker.beginFrame();
            const bool readable = gate.isReadable();
            if (readable) {
                // Several reads spread across the frame, like a UI window
                // touching handle + width + height at different points.
                for (int pass = 0; pass < 3; ++pass) {
                    const uint64_t first = payload[0];
                    for (int w = 1; w < PAYLOAD_WORDS; ++w) {
                        if (payload[w] != first) {
                            torn.store(true, std::memory_order_relaxed);
                        }
                    }
                    std::this_thread::yield();
                }
            }
            tracker.endFrame();
        }
    });

    std::thread writer([&] {
        for (uint64_t v = 1; v <= MUTATIONS; ++v) {
            gate.close(tracker);
            while (!gate.mayMutate(tracker)) {
                if (torn.load(std::memory_order_relaxed)) return;
                std::this_thread::yield();
            }
            // No GPU here; waitForGpu() in the real path only covers
            // submitted command lists, which the protocol already excludes
            // from ever holding the payload. Mutate non-atomically.
            for (int w = 0; w < PAYLOAD_WORDS; ++w) {
                payload[w] = v;
                if ((w & 1) == 0) std::this_thread::yield();
            }
            gate.open();
        }
        writerDone.store(true, std::memory_order_relaxed);
    });

    writer.join();
    editor.join();

    EXPECT_FALSE(torn.load()) << "a frame permitted to read observed a "
                                 "partially-mutated payload";
}

} // namespace
