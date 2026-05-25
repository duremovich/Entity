#include "entity/media/DecodedFrame.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

using entity::DecodedFrame;

// ---------------------------------------------------------------------------
// Storage accessor — CpuHeap path
// ---------------------------------------------------------------------------

TEST(DecodedFrameTest, DefaultStorageIsCpuHeap) {
    DecodedFrame f;
    EXPECT_EQ(f.storage, DecodedFrame::Storage::CpuHeap);
}

TEST(DecodedFrameTest, SizeReturnsZeroOnDefaultConstruct) {
    DecodedFrame f;
    EXPECT_EQ(f.size(), 0u);
}

TEST(DecodedFrameTest, DataReturnsNullOnDefaultConstruct) {
    const DecodedFrame f;
    EXPECT_EQ(f.data(), nullptr);
}

TEST(DecodedFrameTest, AllocateSetsSizeCpuHeap) {
    DecodedFrame f;
    f.allocate(16, 16, entity::TextureFormat::RGBA8_UNORM);
    EXPECT_EQ(f.size(), 16u * 16u * 4u);
    EXPECT_EQ(f.storage, DecodedFrame::Storage::CpuHeap);
}

TEST(DecodedFrameTest, MutableDataMatchesDataAfterAllocate) {
    DecodedFrame f;
    f.allocate(4, 4);
    EXPECT_NE(f.mutableData(), nullptr);
    EXPECT_EQ(f.mutableData(), f.cpuData.data());
}

TEST(DecodedFrameTest, DataPointerMatchesCpuDataOnCpuHeap) {
    DecodedFrame f;
    f.allocate(8, 8);
    const DecodedFrame& cf = f;
    EXPECT_EQ(cf.data(), f.cpuData.data());
}

TEST(DecodedFrameTest, MutableDataWriteIsReadBackViaData) {
    DecodedFrame f;
    f.allocate(2, 2); // 4 pixels × 4 bytes = 16 bytes
    uint8_t* wr = f.mutableData();
    ASSERT_NE(wr, nullptr);
    wr[0] = 0xAB;
    wr[1] = 0xCD;
    const uint8_t* rd = f.data();
    EXPECT_EQ(rd[0], 0xABu);
    EXPECT_EQ(rd[1], 0xCDu);
}

// ---------------------------------------------------------------------------
// Storage accessor — UploadHeap path (null uploadBuf = safe no-crash)
// ---------------------------------------------------------------------------

TEST(DecodedFrameTest, UploadHeapNullBufDataReturnsNull) {
    DecodedFrame f;
    f.storage = DecodedFrame::Storage::UploadHeap;
    // uploadBuf is null (no pool available in unit test); accessor must not crash
    EXPECT_EQ(f.data(), nullptr);
    EXPECT_EQ(f.mutableData(), nullptr);
    EXPECT_EQ(f.size(), 0u);
}

// ---------------------------------------------------------------------------
// allocate() always produces CpuHeap storage
// ---------------------------------------------------------------------------

TEST(DecodedFrameTest, AllocateOverwritesUploadHeapStorage) {
    DecodedFrame f;
    f.storage = DecodedFrame::Storage::UploadHeap;
    f.allocate(4, 4);
    // allocate() unconditionally switches to CpuHeap
    EXPECT_EQ(f.storage, DecodedFrame::Storage::CpuHeap);
    EXPECT_EQ(f.size(), 4u * 4u * 4u);
    EXPECT_NE(f.data(), nullptr);
}

// ---------------------------------------------------------------------------
// clear() resets to CpuHeap, zeroes all metadata
// ---------------------------------------------------------------------------

TEST(DecodedFrameTest, ClearResetsStorageAndMetadata) {
    DecodedFrame f;
    f.allocate(8, 8);
    f.frameNumber = 42;
    f.width = 8;
    f.height = 8;
    f.valid.store(true);
    f.clear();

    EXPECT_EQ(f.storage, DecodedFrame::Storage::CpuHeap);
    EXPECT_EQ(f.size(), 0u);
    // data() returns cpuData.data() which may be non-null on an empty vector
    // (implementation-defined); size() == 0 is the authoritative "empty" check.
    EXPECT_EQ(f.frameNumber, -1);
    EXPECT_EQ(f.width, 0u);
    EXPECT_EQ(f.height, 0u);
    EXPECT_FALSE(f.valid.load());
}

// ---------------------------------------------------------------------------
// Copy / move semantics — storage field is preserved
// ---------------------------------------------------------------------------

TEST(DecodedFrameTest, CopyConstructorPreservesStorageAndData) {
    DecodedFrame src;
    src.allocate(4, 4);
    src.mutableData()[0] = 0x55;
    src.frameNumber = 7;

    DecodedFrame dst(src);
    EXPECT_EQ(dst.storage, DecodedFrame::Storage::CpuHeap);
    EXPECT_EQ(dst.size(), src.size());
    EXPECT_EQ(dst.data()[0], 0x55u);
    EXPECT_EQ(dst.frameNumber, 7);
}

TEST(DecodedFrameTest, MoveConstructorTransfersData) {
    DecodedFrame src;
    src.allocate(4, 4);
    src.mutableData()[3] = 0x77;
    const uint8_t* originalPtr = src.cpuData.data();

    DecodedFrame dst(std::move(src));
    EXPECT_EQ(dst.storage, DecodedFrame::Storage::CpuHeap);
    // After move, dst.cpuData holds the original allocation (same pointer)
    EXPECT_EQ(dst.cpuData.data(), originalPtr);
    EXPECT_EQ(dst.data()[3], 0x77u);
    // src is now empty
    EXPECT_EQ(src.size(), 0u);
}

TEST(DecodedFrameTest, MoveAssignmentTransfersData) {
    DecodedFrame src;
    src.allocate(4, 4);
    src.mutableData()[0] = 0x99;

    DecodedFrame dst;
    dst = std::move(src);
    EXPECT_EQ(dst.storage, DecodedFrame::Storage::CpuHeap);
    EXPECT_EQ(dst.data()[0], 0x99u);
    EXPECT_EQ(src.size(), 0u);
}

// ---------------------------------------------------------------------------
// bytesFor — static helper (unchanged from before, regression guard)
// ---------------------------------------------------------------------------

TEST(DecodedFrameTest, BytesForRGBA8) {
    EXPECT_EQ(DecodedFrame::bytesFor(4, 4, entity::TextureFormat::RGBA8_UNORM), 64u);
    EXPECT_EQ(DecodedFrame::bytesFor(1, 1, entity::TextureFormat::RGBA8_UNORM), 4u);
}

TEST(DecodedFrameTest, BytesForBC1) {
    // 4×4 RGBA8 compressed into one 8-byte BC1 block
    EXPECT_EQ(DecodedFrame::bytesFor(4, 4, entity::TextureFormat::BC1_UNORM), 8u);
    // 8×8 = four 4×4 blocks × 8 bytes each
    EXPECT_EQ(DecodedFrame::bytesFor(8, 8, entity::TextureFormat::BC1_UNORM), 32u);
}

TEST(DecodedFrameTest, BytesForBC7) {
    // 4×4 compressed into one 16-byte BC7 block
    EXPECT_EQ(DecodedFrame::bytesFor(4, 4, entity::TextureFormat::BC7_UNORM), 16u);
}
