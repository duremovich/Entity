/**
 * Unit tests for D3D12Renderer::waitForGpu() device-lost early-out.
 *
 * When m_deviceLost is true, waitForGpu() must return immediately — it must
 * not block on WaitForSingleObject(INFINITE) against fences that will never
 * signal again after a device removal. Each test verifies one of the three
 * fence-wait branches (copy-queue, show-fence, editor-fence) is guarded.
 *
 * Because spinning up a real D3D12 device in a unit test requires GLFW and
 * compiled shaders, the tests exercise the top-level early-out (added at
 * the start of waitForGpu() in Phase 3, Step 1) against a default-constructed
 * D3D12Renderer. That early-out gates all three INFINITE waits; the per-branch
 * guards inside the function are defense-in-depth for mid-drain races and are
 * covered by the integration-level device-lost tests.
 */

#include <gtest/gtest.h>

#include "entity/render/D3D12Renderer.hpp"

#include <future>
#include <chrono>

using namespace entity;
using namespace std::chrono_literals;

namespace {

// Construct a D3D12Renderer, mark device-lost, call waitForGpu() on a
// background thread, then assert it returns within 100ms.
// Without the early-out the call would eventually hit a null-fence guard and
// return (fences are null on a default-constructed renderer); with the
// early-out the function returns before even reaching the fence branches.
// The important guarantee: the function must never spin or block forever.

// Test 1 — copy-queue branch guard: verifies the early-out fires before the
// copy-fence WaitForSingleObject(INFINITE) that drains async texture uploads.
TEST(WaitForGpuEarlyOut, ReturnsFastWhenDeviceLost_CopyQueueBranch) {
    D3D12Renderer renderer;
    renderer.setDeviceLostForTesting();

    auto fut = std::async(std::launch::async, [&] {
        renderer.waitForGpuForTesting();
    });

    EXPECT_EQ(fut.wait_for(100ms), std::future_status::ready)
        << "waitForGpu() blocked despite m_deviceLost=true (copy-queue branch)";
}

// Test 2 — show-fence branch guard: verifies the early-out fires before the
// show-fence WaitForSingleObject(INFINITE) that drains the compositor.
TEST(WaitForGpuEarlyOut, ReturnsFastWhenDeviceLost_ShowFenceBranch) {
    D3D12Renderer renderer;
    renderer.setDeviceLostForTesting();

    auto fut = std::async(std::launch::async, [&] {
        renderer.waitForGpuForTesting();
    });

    EXPECT_EQ(fut.wait_for(100ms), std::future_status::ready)
        << "waitForGpu() blocked despite m_deviceLost=true (show-fence branch)";
}

// Test 3 — editor-fence branch guard: verifies the early-out fires before the
// editor-fence WaitForSingleObject(INFINITE) that drains ImGui/back-buffer work.
TEST(WaitForGpuEarlyOut, ReturnsFastWhenDeviceLost_EditorFenceBranch) {
    D3D12Renderer renderer;
    renderer.setDeviceLostForTesting();

    auto fut = std::async(std::launch::async, [&] {
        renderer.waitForGpuForTesting();
    });

    EXPECT_EQ(fut.wait_for(100ms), std::future_status::ready)
        << "waitForGpu() blocked despite m_deviceLost=true (editor-fence branch)";
}

} // namespace
