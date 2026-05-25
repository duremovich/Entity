#pragma once

/**
 * IDecodeBufferAllocator — abstraction over where decoded pixel bytes land.
 *
 * Before Phase 3, DecodeSystem hard-coded one strategy for acquiring pixel
 * buffers: pop a `std::vector<uint8_t>` from `DecodeBufferPool` (CPU heap).
 * Phase 3 introduces the UPLOAD-heap path: decoders write directly into a
 * persistently-mapped D3D12 UPLOAD-heap buffer, eliminating the show-thread
 * memcpy in PlaybackPresenter.
 *
 * The two concrete implementations are:
 *
 *   CpuHeapDecodeBufferAllocator  — wraps DecodeBufferPool; sets
 *       DecodedFrame::storage = CpuHeap and populates cpuData.
 *
 *   UploadHeapDecodeBufferAllocator — wraps UploadHeapBufferPool; sets
 *       DecodedFrame::storage = UploadHeap and populates uploadBuf.
 *
 * Decoders never reference either concrete type; they call
 *   allocator->prepareDecodeBuffer(frame, width, height, format);
 * and then write through frame.mutableData() with the correct row stride.
 *
 * Threading: implementations must be safe to call from the decode worker
 * threads. Both concrete implementations delegate to thread-safe pool APIs.
 */

#include "entity/media/DecodedFrame.hpp"
#include <cstddef>

namespace entity {

class IDecodeBufferAllocator {
public:
    virtual ~IDecodeBufferAllocator() = default;

    // Ensure `frame` has a writable pixel buffer for a texture of the given
    // dimensions and format.
    //
    // Implementations set `frame.storage`, populate the corresponding
    // backing field (cpuData or uploadBuf), and leave the frame ready for
    // decoder writes via `frame.mutableData()`.
    //
    // The upload-heap implementation calls UploadHeapBufferPool::acquireForTexture
    // so the buffer is sized with 256-byte-aligned row pitch (D3D12
    // CopyTextureRegion requirement). Decoders must pass the row pitch from
    // frame.uploadBuf->footprint.Footprint.RowPitch to their write routines
    // (swsScale dstStride, memcpy row stride, etc.) on the UploadHeap path.
    // On the CpuHeap path, tight-packed stride (width * bytesPerPixel) is correct.
    //
    // If preparation fails (e.g. D3D12 resource creation error on the
    // upload-heap path), the implementation falls back to CPU-heap allocation
    // so callers always get a valid buffer. Failure is logged internally.
    virtual void prepareDecodeBuffer(DecodedFrame& frame,
                                     uint32_t      width,
                                     uint32_t      height,
                                     TextureFormat format = TextureFormat::RGBA8_UNORM) = 0;
};

} // namespace entity
