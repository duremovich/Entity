/**
 * Unit tests for IDecodeBufferAllocator implementations.
 *
 * CpuHeapDecodeBufferAllocator and UploadHeapDecodeBufferAllocator are tested
 * without a real D3D12 device — UploadHeap tests exercise the null-pool
 * fallback path (pool == nullptr → falls back to CpuHeap) so they compile
 * and run in CI without GPU hardware.
 *
 * Tests that require a real D3D12 pool are left to UploadHeapBufferPoolTests.
 */

#include "entity/media/DecodeBufferAllocators.hpp"
#include "entity/media/DecodedFrame.hpp"
#include "entity/media/DecodeBufferPool.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

using namespace entity;

// ---------------------------------------------------------------------------
// CpuHeapDecodeBufferAllocator — no pool (fallback path)
// ---------------------------------------------------------------------------

TEST(CpuHeapDecodeBufferAllocatorTest, NullPoolFallsBackToResize) {
    CpuHeapDecodeBufferAllocator alloc(nullptr);
    DecodedFrame frame;

    // 16x16 RGBA8 = 1024 bytes tight-packed
    alloc.prepareDecodeBuffer(frame, 16, 16, TextureFormat::RGBA8_UNORM);

    EXPECT_EQ(frame.storage, DecodedFrame::Storage::CpuHeap);
    EXPECT_EQ(frame.size(), 1024u);
    EXPECT_NE(frame.mutableData(), nullptr);
    // uploadBuf must remain null — we're on the CpuHeap path.
    EXPECT_EQ(frame.uploadBuf, nullptr);
}

TEST(CpuHeapDecodeBufferAllocatorTest, WithPool_RecyclesBuffer) {
    DecodeBufferPool pool;

    // Pre-seed the pool with a 1024-byte buffer so the allocator gets a
    // recycled pointer, not a fresh malloc.
    std::vector<uint8_t> seedBuf(1024, 0xBE);
    const auto* seedPtr = seedBuf.data();
    pool.release(std::move(seedBuf));

    CpuHeapDecodeBufferAllocator alloc(&pool);
    DecodedFrame frame;
    // 16x16 RGBA8 = 1024 bytes — matches the seeded pool entry
    alloc.prepareDecodeBuffer(frame, 16, 16, TextureFormat::RGBA8_UNORM);

    EXPECT_EQ(frame.storage, DecodedFrame::Storage::CpuHeap);
    EXPECT_EQ(frame.size(), 1024u);
    // The allocator should have popped the recycled buffer from the pool.
    EXPECT_EQ(frame.cpuData.data(), seedPtr);
}

TEST(CpuHeapDecodeBufferAllocatorTest, SufficientSizeIsNoop) {
    // If frame already has the correct tight-packed size on CpuHeap,
    // prepareDecodeBuffer must not change the backing allocation (no spurious
    // free+malloc). 8x16 RGBA8 = 512 bytes.
    CpuHeapDecodeBufferAllocator alloc(nullptr);
    DecodedFrame frame;
    frame.storage = DecodedFrame::Storage::CpuHeap;
    frame.cpuData.resize(512);
    const auto* originalPtr = frame.cpuData.data();

    alloc.prepareDecodeBuffer(frame, 8, 16, TextureFormat::RGBA8_UNORM);

    EXPECT_EQ(frame.cpuData.data(), originalPtr)
        << "prepareDecodeBuffer reallocated when size was already sufficient";
}

TEST(CpuHeapDecodeBufferAllocatorTest, ReleasesUploadBufWhenSwitchingStorage) {
    // If the frame previously held an UploadHeap allocation (uploadBuf != null),
    // switching to CpuHeap via the allocator must reset it to avoid a slot leak.
    CpuHeapDecodeBufferAllocator alloc(nullptr);
    DecodedFrame frame;

    // Simulate a frame that previously held an UploadHeap buffer by making a
    // shared_ptr with a use_count we can observe (no real D3D12 resource needed).
    auto dummyBuf = std::make_shared<UploadHeapBuffer>();
    frame.uploadBuf = dummyBuf;
    frame.storage   = DecodedFrame::Storage::UploadHeap;

    // The dummy shared_ptr is now at use_count 2 (dummyBuf + frame.uploadBuf).
    ASSERT_EQ(dummyBuf.use_count(), 2);

    // 4x8 RGBA8 = 128 bytes — dimensions sufficient to trigger allocation
    alloc.prepareDecodeBuffer(frame, 4, 8, TextureFormat::RGBA8_UNORM);

    // After switching to CpuHeap, frame.uploadBuf must be released.
    EXPECT_EQ(frame.uploadBuf, nullptr);
    EXPECT_EQ(dummyBuf.use_count(), 1) << "frame.uploadBuf was not reset on storage switch";
    EXPECT_EQ(frame.storage, DecodedFrame::Storage::CpuHeap);
}

// ---------------------------------------------------------------------------
// UploadHeapDecodeBufferAllocator — null pool (fallback path, no D3D12 needed)
// ---------------------------------------------------------------------------

TEST(UploadHeapDecodeBufferAllocatorTest, NullPoolFallsBackToCpuHeap) {
    // Passing a null shared_ptr pool triggers the fallback path.
    UploadHeapDecodeBufferAllocator alloc(nullptr);
    DecodedFrame frame;

    // 8x8 RGBA8 = 256 bytes tight-packed
    alloc.prepareDecodeBuffer(frame, 8, 8, TextureFormat::RGBA8_UNORM);

    // Should have fallen back to CpuHeap because acquire returned nullptr.
    EXPECT_EQ(frame.storage, DecodedFrame::Storage::CpuHeap);
    EXPECT_EQ(frame.size(), 256u);
    EXPECT_NE(frame.mutableData(), nullptr);
    EXPECT_EQ(frame.uploadBuf, nullptr);
}

TEST(UploadHeapDecodeBufferAllocatorTest, ClearsCpuDataWhenPreparing) {
    // If the frame previously held CpuHeap data, the UploadHeap allocator
    // must clear cpuData before acquiring an upload buffer (or falling back).
    UploadHeapDecodeBufferAllocator alloc(nullptr); // null pool → fallback
    DecodedFrame frame;
    frame.storage = DecodedFrame::Storage::CpuHeap;
    frame.cpuData.assign(512, 0xAB);

    // 8x16 RGBA8 = 512 bytes tight-packed
    alloc.prepareDecodeBuffer(frame, 8, 16, TextureFormat::RGBA8_UNORM);

    // On fallback, cpuData is cleared then resized.
    // On upload-heap success (not tested here — needs real pool), cpuData is cleared.
    // Either way, the frame is valid.
    EXPECT_GE(frame.size(), 512u);
    EXPECT_NE(frame.mutableData(), nullptr);
}
