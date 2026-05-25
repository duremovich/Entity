#pragma once

/**
 * UploadHeapBufferPool — pool of D3D12 UPLOAD-heap-backed persistently-mapped
 * buffers, with GPU-fence safety on recycle.
 *
 * Motivation: PlaybackPresenter::present currently memcpys decoded pixels from
 * CPU heap (FrameCache / DecodeBufferPool) into a per-slot UPLOAD-heap staging
 * buffer on the show thread, then records CopyTextureRegion. Moving decoded
 * pixels directly into UPLOAD-heap buffers (this pool) lets PlaybackPresenter
 * skip the memcpy entirely and record CopyTextureRegion straight from the
 * cache lease. See plan upload-heap-backed-framecache.md for the full design.
 *
 * API mirrors DecodeBufferPool: acquire() / release-via-shared_ptr-deleter.
 * The acquired object is a structured UploadHeapBuffer handle instead of a
 * raw vector.
 *
 * Threading: acquire() and the shared_ptr deleter are called from arbitrary
 * threads (decode workers, show thread). A single mutex protects the bucket
 * map. The deleter fence-waits before recycling, but waits on a per-buffer
 * HANDLE so concurrent drops never steal each other's wakeup.
 *
 * Sizing notes:
 *   - The caller must pass a footprint-aligned size to acquire(bytes). D3D12
 *     requires RowPitch aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT (256).
 *     Use acquireForTexture() which calls ID3D12Device::GetCopyableFootprints
 *     internally and returns both the buffer and the footprint. This saves
 *     every decoder from recomputing the same alignment math.
 *   - UPLOAD-heap resources are persistently mapped once at creation; the
 *     mapped pointer is valid until the resource is destroyed. On destruction
 *     (UploadHeapBuffer::~UploadHeapBuffer) Unmap is called explicitly to keep
 *     the D3D12 debug layer quiet.
 *
 * Fence wiring:
 *   The ctor takes the direct command queue and the show fence. After the show
 *   thread records a CopyTextureRegion that reads from an UploadHeapBuffer, it
 *   must call recordCopyDone(buf, showFenceValue) with the fence value that
 *   will be signaled when that frame's commands complete. The shared_ptr deleter
 *   then fence-waits before returning the buffer to the pool.
 *
 *   The fence value to pass is the value the command queue is about to Signal
 *   (i.e. m_showFenceValues[m_showBackBufferIndex] in D3D12Renderer::endShowFrame
 *   at the time CopyTextureRegion is recorded). See D3D12Renderer.cpp:656-658.
 */

#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace entity {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// UploadHeapBuffer — one D3D12 UPLOAD-heap resource, persistently mapped.
//
// Lifetime: created by UploadHeapBufferPool::acquire(); destroyed either when
// the pool is torn down (pool holds the ComPtr and calls reset()) or when the
// pool is already gone and the shared_ptr deleter falls through to the dtor.
// The destructor Unmaps and CloseHandles — the ComPtr then drops the resource.
//
// Do NOT create UploadHeapBuffer directly; always obtain via the pool.
// ---------------------------------------------------------------------------
struct UploadHeapBuffer {
    ComPtr<ID3D12Resource> resource;
    uint64_t    offset{0};          // always 0 in v1 (one resource per buffer)
    uint8_t*    mappedPtr{nullptr}; // persistent map; valid while resource lives
    size_t      size{0};
    uint64_t    lastCopyFenceValue{0}; // bumped by recordCopyDone()
    HANDLE      fenceEvent{nullptr};   // per-buffer auto-reset event; avoids
                                       // concurrent-SetEventOnCompletion data race
    // Footprint populated by acquireForTexture(). Row pitch is 256-byte aligned
    // (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT). Decoders must write with
    // Footprint.RowPitch as their row stride. acquireForTexture always sets this;
    // plain acquire() leaves it zeroed (no texture footprint context).
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};

    UploadHeapBuffer() = default;
    ~UploadHeapBuffer();

    // Non-copyable; the ComPtr and HANDLE are unique resources.
    UploadHeapBuffer(const UploadHeapBuffer&) = delete;
    UploadHeapBuffer& operator=(const UploadHeapBuffer&) = delete;
    UploadHeapBuffer(UploadHeapBuffer&&) = delete;
    UploadHeapBuffer& operator=(UploadHeapBuffer&&) = delete;
};

// ---------------------------------------------------------------------------
// UploadHeapBufferPool
// ---------------------------------------------------------------------------
class UploadHeapBufferPool : public std::enable_shared_from_this<UploadHeapBufferPool> {
public:
    // device       — the D3D12 device used to create UPLOAD-heap resources.
    // commandQueue — the direct command queue (NOT the COPY queue). In this
    //                codebase (ADR-0014) the show thread records CopyTextureRegion
    //                on the direct queue and pairs with the show fence. Pass
    //                m_gpu->commandQueue() from D3D12Renderer.
    // fence        — the show fence (m_showFence). The deleter calls
    //                fence->GetCompletedValue() and, if needed, fence->
    //                SetEventOnCompletion to stall before recycle.
    // maxBuffersPerSize — per-size-bucket cap. Buffers beyond this limit are freed
    //                     on release instead of recycled.
    // maxTotal          — global cap across all size buckets. When exceeded on
    //                     release, the oldest pool entry across all buckets is
    //                     evicted to make room.
    UploadHeapBufferPool(ID3D12Device*       device,
                         ID3D12CommandQueue* commandQueue, // direct queue, paired with show fence
                         ID3D12Fence*        fence,        // show fence
                         size_t maxBuffersPerSize = 8,
                         size_t maxTotal = 32);

    ~UploadHeapBufferPool();

    // Non-copyable; holds raw D3D12 pointers and bucket state.
    UploadHeapBufferPool(const UploadHeapBufferPool&) = delete;
    UploadHeapBufferPool& operator=(const UploadHeapBufferPool&) = delete;

    // Acquire a mapped buffer of exactly `bytes`. The caller must pass a
    // footprint-aligned size (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256 per
    // row). Returns nullptr on D3D12 resource-creation failure; caller must
    // fall back to the CPU-heap path in that case.
    //
    // When the returned shared_ptr's ref-count drops to zero the deleter runs:
    // it fence-waits (if lastCopyFenceValue > completed value) then returns
    // the buffer to the pool (or frees it if the pool is gone).
    std::shared_ptr<UploadHeapBuffer> acquire(size_t bytes);

    // Convenience: computes the D3D12 footprint for a texture of the given
    // dimensions + format via GetCopyableFootprints, then calls acquire() with
    // the resulting TotalBytes. Writes the footprint (including RowPitch the
    // decoder needs for strided writes) into *outFootprint.
    // Returns nullptr on device failure or if GetCopyableFootprints fails.
    std::shared_ptr<UploadHeapBuffer> acquireForTexture(
        UINT            width,
        UINT            height,
        DXGI_FORMAT     format,
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT* outFootprint);

    // Call this on the show thread, AFTER recording the CopyTextureRegion that
    // reads from `buf`, with the fence value that the command queue will Signal
    // when this frame's commands complete. The shared_ptr deleter will then wait
    // on that value before recycling the buffer.
    //
    // fenceValueAfterCopy: the value being signaled this frame, i.e.
    //   m_showFenceValues[m_showBackBufferIndex] before the increment in
    //   D3D12Renderer::endShowFrame (line ~657). The pool stores it in
    //   buf.lastCopyFenceValue.
    void recordCopyDone(UploadHeapBuffer& buf, uint64_t fenceValueAfterCopy);

    // Diagnostics.
    size_t entries() const;
    size_t bucketCount() const;

    // Enqueue a raw UploadHeapBuffer* onto the deferred-recycle queue.
    // Called from the shared_ptr deleter when the GPU fence has NOT yet
    // been signaled — the deleter must not block (show thread), so it
    // hands the buffer off to m_recycleWorker which does the fence-wait
    // off the hot path. Takes raw ownership of the pointer.
    //
    // The pool check in the deleter guarantees this is called only when
    // weak_ptr.lock() succeeds — i.e. the pool is still alive.
    void deferRelease(UploadHeapBuffer* p);

    // Signal the recycle worker to drain remaining deferred entries and
    // exit. Called implicitly by the destructor; exposed for testing.
    void stop();

private:
    // Allocate a fresh UploadHeapBuffer from the D3D12 device. Returns nullptr
    // on failure. Called from acquire() on a bucket miss.
    std::unique_ptr<UploadHeapBuffer> allocateFresh(size_t bytes);

    // Return a buffer to the pool. Called from the shared_ptr deleter (fast
    // path, GPU already done) or from the recycle worker (slow path, after
    // fence-wait). If the pool is gone (weak_ptr expired) the buffer falls
    // through to plain destruction (the unique_ptr in the deleter simply drops).
    void release(std::unique_ptr<UploadHeapBuffer> buf);

    // Background worker: pops from m_deferQueue, waits on each buffer's fence,
    // then calls release(). Exits when m_stopRequested is set and the queue is
    // drained.
    void recycleWorkerFunc();

    ID3D12Device*       m_device{nullptr};
    ID3D12CommandQueue* m_commandQueue{nullptr}; // direct queue
    ID3D12Fence*        m_fence{nullptr};        // show fence

    mutable std::mutex m_mutex;

    // Buckets keyed on byte size. Each bucket holds recycled buffers ready for
    // reuse. The unique_ptr owns the UploadHeapBuffer's D3D12 resource while
    // it waits in the pool; on acquire() ownership transfers to the shared_ptr
    // deleter via a release/re-wrap dance.
    std::unordered_map<size_t, std::vector<std::unique_ptr<UploadHeapBuffer>>> m_buckets;

    // For the global-cap eviction path we need to know which bucket/entry to
    // drop. We track insertion order via a simple list of (size, index) pairs
    // parallel to the buckets. When the global cap is exceeded the front entry
    // is evicted.
    struct BucketKey {
        size_t size;
    };
    std::vector<BucketKey> m_insertionOrder; // oldest first

    size_t m_maxBuffersPerSize;
    size_t m_maxTotal;
    size_t m_totalEntries{0};

    // Deferred-recycle worker — fence-waits on slow-path drops off the
    // show thread.
    std::queue<UploadHeapBuffer*> m_deferQueue;   // raw owning ptrs
    std::mutex                    m_deferMutex;
    std::condition_variable       m_deferCv;
    std::atomic<bool>             m_stopRequested{false};
    std::thread                   m_recycleWorker;
};

} // namespace entity
