/**
 * UploadHeapBufferPool unit tests.
 *
 * Tests 1-4 use a real D3D12 device + fence. The entire file is compiled only
 * when ENTITY_HAVE_D3D12_TESTS is defined (CMake option, default OFF).
 * Each test calls GTEST_SKIP() if D3D12CreateDevice fails (headless CI / no GPU).
 *
 * Test 5 exercises the fence-wait path by signaling a fence value and
 * verifying the deleter observes it.
 */

#if defined(ENTITY_HAVE_D3D12_TESTS)

#include "entity/render/UploadHeapBufferPool.hpp"

#include <gtest/gtest.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstring>
#include <memory>

using Microsoft::WRL::ComPtr;
using namespace entity;

// ---------------------------------------------------------------------------
// Test fixture: minimal D3D12 device + direct queue + fence.
//
// All pool tests share a single device/queue/fence created in SetUpTestSuite
// so we pay the D3D12CreateDevice cost once per binary run rather than once
// per test. If device creation fails we skip the whole suite.
// ---------------------------------------------------------------------------

namespace {

ComPtr<ID3D12Device>       g_device;
ComPtr<ID3D12CommandQueue> g_commandQueue;
ComPtr<ID3D12Fence>        g_fence;
HANDLE                     g_fenceEvent{nullptr};
std::atomic<uint64_t>      g_nextFenceValue{1};
bool                       g_d3dAvailable{false};

void InitD3D() {
    // Try hardware adapter first; fall back to WARP.
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;

    // Attempt hardware device first.
    HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&g_device));
    if (FAILED(hr)) {
        // Try WARP (software rasterizer).
        ComPtr<IDXGIAdapter> warpAdapter;
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)))) {
            hr = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                   IID_PPV_ARGS(&g_device));
        }
    }
    if (FAILED(hr) || !g_device) return;

    D3D12_COMMAND_QUEUE_DESC qDesc = {};
    qDesc.Type  = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    hr = g_device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&g_commandQueue));
    if (FAILED(hr)) return;

    hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (FAILED(hr)) return;

    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent) return;

    g_d3dAvailable = true;
}

void TearDownD3D() {
    if (g_fenceEvent) {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }
    g_fence.Reset();
    g_commandQueue.Reset();
    g_device.Reset();
    g_d3dAvailable = false;
}

} // namespace

// ---------------------------------------------------------------------------
// SetUpTestSuite / TearDownTestSuite — called once for the whole suite.
// ---------------------------------------------------------------------------

class UploadHeapBufferPoolTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        InitD3D();
    }
    static void TearDownTestSuite() {
        TearDownD3D();
    }

    void SetUp() override {
        if (!g_d3dAvailable) {
            GTEST_SKIP() << "D3D12 device not available; skipping GPU test.";
        }
    }

    // Helper: create a pool wrapping the global device/queue/fence.
    std::shared_ptr<UploadHeapBufferPool> makePool(
        size_t maxPerSize = 8, size_t maxTotal = 32)
    {
        return std::make_shared<UploadHeapBufferPool>(
            g_device.Get(),
            g_commandQueue.Get(),
            g_fence.Get(),
            maxPerSize,
            maxTotal);
    }
};

// ---------------------------------------------------------------------------
// Test 1 — acquire(N) returns a buffer with size == N and a writable mappedPtr.
// ---------------------------------------------------------------------------

TEST_F(UploadHeapBufferPoolTest, AcquireReturnsWritableBuffer) {
    auto pool = makePool();

    constexpr size_t kBytes = 1024;
    auto buf = pool->acquire(kBytes);

    ASSERT_NE(buf, nullptr);
    EXPECT_EQ(buf->size, kBytes);
    ASSERT_NE(buf->mappedPtr, nullptr);

    // Verify the pointer is genuinely writable.
    std::memset(buf->mappedPtr, 0xAB, kBytes);
    EXPECT_EQ(buf->mappedPtr[0],       static_cast<uint8_t>(0xAB));
    EXPECT_EQ(buf->mappedPtr[kBytes-1], static_cast<uint8_t>(0xAB));
}

// ---------------------------------------------------------------------------
// Test 2 — dropping the shared_ptr → pool entry goes up; re-acquire returns
// the same mappedPtr (recycling).
// ---------------------------------------------------------------------------

TEST_F(UploadHeapBufferPoolTest, RecyclesBufferOnRelease) {
    auto pool = makePool();

    constexpr size_t kBytes = 2048;
    uint8_t* originalPtr = nullptr;

    {
        auto buf = pool->acquire(kBytes);
        ASSERT_NE(buf, nullptr);
        originalPtr = buf->mappedPtr;
        EXPECT_EQ(pool->entries(), 0u); // held, not in pool yet
    }
    // shared_ptr dropped; lastCopyFenceValue == 0, no fence wait needed.
    EXPECT_EQ(pool->entries(), 1u);

    // Re-acquire should return the recycled buffer (same mapped pointer).
    auto buf2 = pool->acquire(kBytes);
    ASSERT_NE(buf2, nullptr);
    EXPECT_EQ(buf2->mappedPtr, originalPtr) << "Expected recycled buffer (same mapped ptr)";
    EXPECT_EQ(pool->entries(), 0u);
}

// ---------------------------------------------------------------------------
// Test 3 — acquire(N) × (maxBuffersPerSize+1) → bucket cap respected.
// ---------------------------------------------------------------------------

TEST_F(UploadHeapBufferPoolTest, PerBucketCapHonored) {
    const size_t cap = 3;
    auto pool = makePool(/*maxPerSize=*/cap, /*maxTotal=*/100);

    constexpr size_t kBytes = 4096;

    // Hold cap+1 buffers, then release them all.
    {
        std::vector<std::shared_ptr<UploadHeapBuffer>> bufs;
        for (size_t i = 0; i < cap + 1; ++i) {
            auto b = pool->acquire(kBytes);
            ASSERT_NE(b, nullptr);
            bufs.push_back(std::move(b));
        }
        // Drop all — cap+1 tries to enter the bucket.
    }

    // Only `cap` should be kept; the extra one is freed.
    EXPECT_EQ(pool->entries(), cap);
}

// ---------------------------------------------------------------------------
// Test 4 — acquire(N) × (maxTotal+1) → global cap respected; pool entries
// bounded at maxTotal.
// ---------------------------------------------------------------------------

TEST_F(UploadHeapBufferPoolTest, GlobalCapHonored) {
    const size_t maxTotal = 4;
    auto pool = makePool(/*maxPerSize=*/100, /*maxTotal=*/maxTotal);

    // Release maxTotal+1 buffers of distinct sizes so each lands in its own
    // bucket (side-stepping the per-bucket cap).
    constexpr size_t kBase = 8192;
    {
        std::vector<std::shared_ptr<UploadHeapBuffer>> bufs;
        for (size_t i = 0; i < maxTotal + 1; ++i) {
            auto b = pool->acquire(kBase + i * 256); // distinct sizes
            ASSERT_NE(b, nullptr);
            bufs.push_back(std::move(b));
        }
    }

    // Pool should hold at most maxTotal entries.
    EXPECT_LE(pool->entries(), maxTotal);
}

// ---------------------------------------------------------------------------
// Test 5 — recordCopyDone + drop with unsignaled fence → deferred recycle
// worker eventually returns the buffer to the pool once the fence is signaled.
//
// Strategy: signal the fence to V-1 (GetCompletedValue() < V), attach a
// recordCopyDone with value V, drop the shared_ptr. The deleter is now
// non-blocking (Part B): it enqueues onto m_deferQueue and returns immediately.
// The pool's recycle worker thread does the actual fence-wait and calls
// release(). Signal the fence past V, then poll until pool->entries() == 1
// with a wall-clock deadline — join() only syncs with the drop thread, not
// with the worker thread, so a bare post-join assertion would race.
// ---------------------------------------------------------------------------

TEST_F(UploadHeapBufferPoolTest, DeferredRecycleHappensAfterFenceSignal) {
    auto pool = makePool();

    // Pick fence values that don't collide with other tests.
    const uint64_t prevValue  = g_nextFenceValue.fetch_add(2);
    const uint64_t copyValue  = prevValue + 1;
    const uint64_t afterValue = prevValue + 2;

    // Advance the fence to prevValue — so GetCompletedValue() == prevValue < copyValue.
    HRESULT hr = g_commandQueue->Signal(g_fence.Get(), prevValue);
    ASSERT_HRESULT_SUCCEEDED(hr);
    if (g_fence->GetCompletedValue() < prevValue) {
        g_fence->SetEventOnCompletion(prevValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 5000);
    }
    ASSERT_GE(g_fence->GetCompletedValue(), prevValue);

    constexpr size_t kBytes = 1024;

    // Acquire, mark with copyValue, drop on a background thread.
    auto buf = pool->acquire(kBytes);
    ASSERT_NE(buf, nullptr);
    pool->recordCopyDone(*buf, copyValue);

    std::atomic<bool> deleterDone{false};
    std::thread dropThread([b = std::move(buf), &deleterDone]() mutable {
        b.reset(); // non-blocking: enqueues to deferQueue and returns immediately
        deleterDone.store(true, std::memory_order_release);
    });

    // Give the drop thread a moment to start and enqueue.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Signal the fence past copyValue so the recycle worker unblocks.
    hr = g_commandQueue->Signal(g_fence.Get(), afterValue);
    ASSERT_HRESULT_SUCCEEDED(hr);

    dropThread.join();
    EXPECT_TRUE(deleterDone.load());

    // Poll until the recycle worker has called release() and the buffer is
    // back in the pool. join() only synchronized with the drop thread; the
    // worker thread runs independently and may still be doing its fence-wait.
    // 2 s deadline is generous — the worker should complete in <50ms in practice.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (pool->entries() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_EQ(pool->entries(), 1u);
}

// ---------------------------------------------------------------------------
// Test 6 — acquireForTexture fills in the footprint correctly.
// ---------------------------------------------------------------------------

TEST_F(UploadHeapBufferPoolTest, AcquireForTexturePopulatesFootprint) {
    auto pool = makePool();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    auto buf = pool->acquireForTexture(1920, 1080, DXGI_FORMAT_R8G8B8A8_UNORM, &footprint);

    ASSERT_NE(buf, nullptr);

    // RowPitch must be aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256).
    EXPECT_EQ(footprint.Footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT, 0u);
    EXPECT_EQ(footprint.Footprint.Width,  1920u);
    EXPECT_EQ(footprint.Footprint.Height, 1080u);

    // The buffer must be at least as large as the footprint-aligned allocation.
    const size_t minBytes = static_cast<size_t>(footprint.Footprint.RowPitch) * 1080u;
    EXPECT_GE(buf->size, minBytes);
}

// ---------------------------------------------------------------------------
// Test 7 — Non-blocking deleter: dropping a buffer with an unsignaled fence
// does NOT block the calling thread; the buffer eventually appears in the pool.
//
// Strategy: acquire a buffer, recordCopyDone with a fence value ahead of the
// current completed value, then drop the shared_ptr on the calling thread and
// assert that the thread returns quickly (< 2 s). Separately signal the fence
// past that value and poll until the pool entry appears (the recycle worker
// will have done the fence-wait off-thread).
// ---------------------------------------------------------------------------

TEST_F(UploadHeapBufferPoolTest, DeleterDoesNotBlockOnUnsignaledFence) {
    auto pool = makePool();

    // Pick fence values well ahead of the current completed value.
    const uint64_t base      = g_nextFenceValue.fetch_add(3);
    const uint64_t prevValue = base;
    const uint64_t copyValue = base + 1;
    const uint64_t sigValue  = base + 2;

    // Advance fence to prevValue so GetCompletedValue() == prevValue < copyValue.
    HRESULT hr = g_commandQueue->Signal(g_fence.Get(), prevValue);
    ASSERT_HRESULT_SUCCEEDED(hr);
    if (g_fence->GetCompletedValue() < prevValue) {
        g_fence->SetEventOnCompletion(prevValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 5000);
    }
    ASSERT_GE(g_fence->GetCompletedValue(), prevValue);

    constexpr size_t kBytes = 1024;
    auto buf = pool->acquire(kBytes);
    ASSERT_NE(buf, nullptr);
    pool->recordCopyDone(*buf, copyValue);

    // Drop on the CALLING thread and measure elapsed time.
    auto t0 = std::chrono::steady_clock::now();
    buf.reset(); // must return quickly — slow path defers to recycle worker
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // The calling thread must not have blocked for anything close to the 5 s
    // timeout. 500 ms is a generous ceiling on a loaded CI machine.
    EXPECT_LT(elapsed, 500) << "Deleter blocked the calling thread for " << elapsed << " ms";

    // Now signal the fence past copyValue so the recycle worker unblocks.
    hr = g_commandQueue->Signal(g_fence.Get(), sigValue);
    ASSERT_HRESULT_SUCCEEDED(hr);

    // Poll until the pool has the entry (the worker may need a few ms).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (pool->entries() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(pool->entries(), 1u) << "Recycle worker did not return buffer to pool";
}

// ---------------------------------------------------------------------------
// Test 8 — When the pool is destroyed (stop() called by dtor), any buffers
// still in the deferred queue are drained and destroyed without being recycled.
// No crash, no leak (verified by GTest's exit-code check).
// ---------------------------------------------------------------------------

TEST_F(UploadHeapBufferPoolTest, WorkerDrainsDeferredQueueOnShutdown) {
    auto pool = makePool();

    // Pick fence values ahead of current completed value so the deferred path fires.
    const uint64_t base      = g_nextFenceValue.fetch_add(3);
    const uint64_t prevValue = base;
    const uint64_t copyValue = base + 1;
    const uint64_t sigValue  = base + 2;

    HRESULT hr = g_commandQueue->Signal(g_fence.Get(), prevValue);
    ASSERT_HRESULT_SUCCEEDED(hr);
    if (g_fence->GetCompletedValue() < prevValue) {
        g_fence->SetEventOnCompletion(prevValue, g_fenceEvent);
        WaitForSingleObject(g_fenceEvent, 5000);
    }
    ASSERT_GE(g_fence->GetCompletedValue(), prevValue);

    constexpr size_t kBytes = 2048;
    auto buf = pool->acquire(kBytes);
    ASSERT_NE(buf, nullptr);
    pool->recordCopyDone(*buf, copyValue);
    buf.reset(); // enqueues to deferred queue; recycle worker blocks waiting for fence

    // Signal past copyValue so the worker doesn't hang at shutdown.
    hr = g_commandQueue->Signal(g_fence.Get(), sigValue);
    ASSERT_HRESULT_SUCCEEDED(hr);

    // Destroy the pool. The dtor calls stop(), which joins the worker thread
    // after it drains and destroys the deferred buffer. Must not deadlock or crash.
    pool.reset();
    SUCCEED(); // if we got here without deadlock, the test passes
}

#endif // ENTITY_HAVE_D3D12_TESTS
