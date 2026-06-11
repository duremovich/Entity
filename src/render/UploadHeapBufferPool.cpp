#include "entity/render/UploadHeapBufferPool.hpp"
#include "entity/profile/Tracy.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <iostream>

namespace entity {

// Monotonic id for DRED resource naming. Pooled buffers outlive the call that
// allocated them, so a unique-ish id lets a page-fault allocation node name the
// guilty buffer + its size class.
static std::atomic<uint32_t> g_uploadPoolBufId{0};

// ---------------------------------------------------------------------------
// UploadHeapBuffer destructor
// ---------------------------------------------------------------------------

UploadHeapBuffer::~UploadHeapBuffer() {
    // Unmap before releasing the resource; keeps the D3D12 debug layer quiet.
    if (resource && mappedPtr) {
        resource->Unmap(0, nullptr);
        mappedPtr = nullptr;
    }
    // resource ComPtr releases the D3D12 allocation automatically.

    if (fenceEvent) {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
    }
}

// ---------------------------------------------------------------------------
// UploadHeapBufferPool
// ---------------------------------------------------------------------------

UploadHeapBufferPool::UploadHeapBufferPool(ID3D12Device*       device,
                                           ID3D12CommandQueue* commandQueue,
                                           ID3D12Fence*        fence,
                                           size_t              maxBuffersPerSize,
                                           size_t              maxTotal)
    : m_device{device}
    , m_commandQueue{commandQueue}
    , m_fence{fence}
    , m_maxBuffersPerSize{maxBuffersPerSize}
    , m_maxTotal{maxTotal}
{
    assert(device       && "UploadHeapBufferPool: device must not be null");
    assert(commandQueue && "UploadHeapBufferPool: commandQueue must not be null");
    assert(fence        && "UploadHeapBufferPool: fence must not be null");

    // Spawn the deferred-recycle worker. It runs until stop() is called
    // (from the dtor), draining any remaining entries at exit.
    m_recycleWorker = std::thread(&UploadHeapBufferPool::recycleWorkerFunc, this);
}

UploadHeapBufferPool::~UploadHeapBufferPool() {
    // Signal the worker and wait for it to drain the deferred queue.
    stop();

    // All outstanding shared_ptrs hold a weak_ptr back to this pool. If any
    // are still alive when we're destroyed, their deleters will fail to lock
    // the weak_ptr and will simply destroy the UploadHeapBuffer (GPU resource
    // freed via ComPtr + Unmap in the dtor). No dangling references to us.
    std::lock_guard<std::mutex> lk(m_mutex);
    m_buckets.clear();        // destroys all pooled UploadHeapBuffers
    m_insertionOrder.clear();
    m_totalEntries = 0;
}

// ---------------------------------------------------------------------------
// acquire
// ---------------------------------------------------------------------------

std::shared_ptr<UploadHeapBuffer> UploadHeapBufferPool::acquire(size_t bytes) {
    if (bytes == 0) return nullptr;

    std::unique_ptr<UploadHeapBuffer> buf;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_buckets.find(bytes);
        if (it != m_buckets.end() && !it->second.empty()) {
            // Cache hit: pop from the back of this bucket.
            buf = std::move(it->second.back());
            it->second.pop_back();
            --m_totalEntries;

            // Remove the corresponding entry from insertion order tracking.
            // Scan from the front (oldest) to find a matching size entry that
            // no longer exists in the bucket — we just popped one.
            for (auto oi = m_insertionOrder.begin(); oi != m_insertionOrder.end(); ++oi) {
                if (oi->size == bytes) {
                    m_insertionOrder.erase(oi);
                    break;
                }
            }
        }
    }

    if (!buf) {
        // Cache miss: allocate fresh (outside the lock — no need to hold it
        // during the D3D12 CreateCommittedResource call).
        buf = allocateFresh(bytes);
        if (!buf) return nullptr;
    }

    // Wrap in a shared_ptr with a custom deleter that fence-waits then recycles.
    // The deleter captures a weak_ptr to this pool so it safely handles the
    // "pool was already destroyed" case.
    auto* rawBuf = buf.release(); // transfer ownership to the deleter

    // Capture fence raw pointer — safe because the pool owns the pool's
    // lifetime; if the pool is gone the weak_ptr guard below prevents the
    // fence access. But even then, the fence itself is owned by D3D12Renderer
    // which outlives any outstanding UploadHeapBuffer via the same ownership
    // chain. To be safe, capture both the fence and a weak_ptr to the pool.
    std::weak_ptr<UploadHeapBufferPool> weakSelf = shared_from_this();

    // NOTE: shared_from_this() is valid because UploadHeapBufferPool must be
    // held in a shared_ptr by callers. That is enforced by construction — see
    // Engine.cpp which will construct this via make_shared.

    auto deleter = [weakSelf, fence = m_fence](UploadHeapBuffer* p) {
        if (!p) return;

        // Fast path: GPU is already past lastCopyFenceValue (or the buffer was
        // never used in a GPU copy) — recycle inline without blocking.
        //
        // Slow path: GPU hasn't signaled yet — hand off to the pool's
        // background recycle worker via deferRelease(). The worker does the
        // fence-wait off the show thread.
        //
        // Device-removed guard: GetCompletedValue() returns UINT64_MAX when
        // the device is lost. A dead-device buffer must never be recycled.
        if (fence && p->lastCopyFenceValue > 0) {
            const uint64_t completed = fence->GetCompletedValue();

            if (completed == UINT64_MAX) {
                // Device removed — destroy without recycling.
                std::cerr << "[UploadHeapBufferPool] Pool deleter: device removed "
                             "(UINT64_MAX sentinel); destroying buffer instead of recycling.\n";
                delete p;
                return;
            }

            if (completed < p->lastCopyFenceValue) {
                // GPU still busy. Defer to the recycle worker so the calling
                // thread (possibly the show thread) doesn't block.
                auto pool = weakSelf.lock();
                if (pool) {
                    pool->deferRelease(p);
                } else {
                    // Pool gone; destroy immediately — no one to recycle into.
                    delete p;
                }
                return;
            }
        }

        // Fast path: fence already signaled (or no GPU copy recorded).
        auto pool = weakSelf.lock();
        if (pool) {
            pool->release(std::unique_ptr<UploadHeapBuffer>(p));
        } else {
            // Pool gone; UploadHeapBuffer dtor (Unmap + ComPtr release) handles cleanup.
            delete p;
        }
    };

    return std::shared_ptr<UploadHeapBuffer>(rawBuf, std::move(deleter));
}

// ---------------------------------------------------------------------------
// acquireForTexture
// ---------------------------------------------------------------------------

std::shared_ptr<UploadHeapBuffer> UploadHeapBufferPool::acquireForTexture(
    UINT            width,
    UINT            height,
    DXGI_FORMAT     format,
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT* outFootprint)
{
    if (!m_device || !outFootprint || width == 0 || height == 0) return nullptr;

    // Describe a 2D texture with 1 mip and 1 subresource to get the footprint.
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width            = width;
    texDesc.Height           = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels        = 1;
    texDesc.Format           = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    texDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    UINT   numRows       = 0;
    UINT64 rowSizeBytes  = 0;
    UINT64 totalBytes    = 0;
    m_device->GetCopyableFootprints(&texDesc, 0, 1, 0,
                                    outFootprint, &numRows, &rowSizeBytes, &totalBytes);

    if (totalBytes == 0) {
        std::cerr << "[UploadHeapBufferPool] GetCopyableFootprints returned 0 bytes"
                     " (w=" << width << " h=" << height << ")\n";
        return nullptr;
    }

    auto buf = acquire(static_cast<size_t>(totalBytes));
    if (buf) {
        // Stamp the footprint so decoders can read RowPitch for strided writes
        // and copyUploadBufferToVideoTextureSlot can use it directly.
        buf->footprint = *outFootprint;
    }
    return buf;
}

// ---------------------------------------------------------------------------
// recordCopyDone
// ---------------------------------------------------------------------------

void UploadHeapBufferPool::recordCopyDone(UploadHeapBuffer& buf,
                                           uint64_t          fenceValueAfterCopy) {
    buf.lastCopyFenceValue = fenceValueAfterCopy;
}

// ---------------------------------------------------------------------------
// entries / bucketCount
// ---------------------------------------------------------------------------

size_t UploadHeapBufferPool::entries() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_totalEntries;
}

size_t UploadHeapBufferPool::bucketCount() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_buckets.size();
}

// ---------------------------------------------------------------------------
// allocateFresh (private)
// ---------------------------------------------------------------------------

std::unique_ptr<UploadHeapBuffer> UploadHeapBufferPool::allocateFresh(size_t bytes) {
    auto buf = std::make_unique<UploadHeapBuffer>();
    buf->size = bytes;

    // Per-buffer fence event (auto-reset). Each UploadHeapBuffer gets its own
    // HANDLE so concurrent drops can call SetEventOnCompletion independently
    // without stealing each other's wakeup. See D3D12Renderer.cpp:1114 for the
    // same rationale ("Never share — concurrent SetEventOnCompletion on the same
    // handle is a data race").
    buf->fenceEvent = CreateEvent(nullptr, /*bManualReset=*/FALSE,
                                  /*bInitialState=*/FALSE, nullptr);
    if (!buf->fenceEvent) {
        std::cerr << "[UploadHeapBufferPool] CreateEvent failed (GLE "
                  << GetLastError() << ")\n";
        return nullptr;
    }

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    resDesc.Width            = static_cast<UINT64>(bytes);
    resDesc.Height           = 1;
    resDesc.DepthOrArraySize = 1;
    resDesc.MipLevels        = 1;
    resDesc.Format           = DXGI_FORMAT_UNKNOWN;
    resDesc.SampleDesc.Count = 1;
    resDesc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resDesc.Flags            = D3D12_RESOURCE_FLAG_NONE;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &resDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, // standard initial state for UPLOAD heap
        nullptr,
        IID_PPV_ARGS(&buf->resource));

    if (FAILED(hr)) {
        std::cerr << "[UploadHeapBufferPool] CreateCommittedResource failed"
                     " for " << bytes << " bytes, HRESULT 0x"
                  << std::hex << hr << std::dec << "\n";
        return nullptr;
    }

    {
        const uint32_t id = g_uploadPoolBufId.fetch_add(1, std::memory_order_relaxed);
        wchar_t name[48];
        swprintf_s(name, L"UploadPoolBuf%u_%zuB", id, bytes);
        buf->resource->SetName(name);
    }

    // Persistent map. readRange = {0,0} tells the runtime we won't read from
    // it on the CPU (UPLOAD heap; driver doesn't need to reverse-map GPU writes).
    D3D12_RANGE readRange = {0, 0};
    void* mapped = nullptr;
    hr = buf->resource->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        std::cerr << "[UploadHeapBufferPool] Map failed for " << bytes
                  << " bytes, HRESULT 0x" << std::hex << hr << std::dec << "\n";
        return nullptr;
    }
    buf->mappedPtr = static_cast<uint8_t*>(mapped);

    return buf;
}

// ---------------------------------------------------------------------------
// release (private) — called from the shared_ptr deleter
// ---------------------------------------------------------------------------

void UploadHeapBufferPool::release(std::unique_ptr<UploadHeapBuffer> buf) {
    if (!buf || buf->size == 0) return;

    // Reset fence value so a recycled buffer doesn't carry stale state.
    buf->lastCopyFenceValue = 0;

    std::lock_guard<std::mutex> lk(m_mutex);

    const size_t sz = buf->size;

    // Global cap: evict the oldest entry across all buckets if needed.
    // This runs first so a full-pool state with available bucket headroom
    // succeeds (eviction frees a slot) rather than hitting the bucket check
    // below and being silently dropped. Global cap is the hard limit;
    // per-bucket cap only guards against one size monopolizing the pool.
    // NOTE: eviction scan is O(m_maxTotal) via insertion-order vector; fine
    // at the default cap of 32, worth a deque if maxTotal grows to 10k+.
    if (m_totalEntries >= m_maxTotal && !m_insertionOrder.empty()) {
        const size_t evictSize = m_insertionOrder.front().size;
        m_insertionOrder.erase(m_insertionOrder.begin());

        auto evit = m_buckets.find(evictSize);
        if (evit != m_buckets.end() && !evit->second.empty()) {
            evit->second.erase(evit->second.begin()); // evict oldest (front)
            --m_totalEntries;
            if (evit->second.empty()) {
                m_buckets.erase(evit);
            }
        }
    }

    // Per-bucket cap — checked after global eviction. If the eviction above
    // freed a slot in this bucket we'd still land here correctly. If eviction
    // targeted a different bucket and this one is still full, drop.
    auto& bucket = m_buckets[sz];
    if (bucket.size() >= m_maxBuffersPerSize) {
        // Bucket full — drop (let the unique_ptr destroy it).
        return;
    }

    m_insertionOrder.push_back({sz});
    bucket.push_back(std::move(buf));
    ++m_totalEntries;
}

// ---------------------------------------------------------------------------
// deferRelease — push a buffer onto the recycle queue (called from deleter)
// ---------------------------------------------------------------------------

void UploadHeapBufferPool::deferRelease(UploadHeapBuffer* p) {
    {
        std::lock_guard<std::mutex> lk(m_deferMutex);
        m_deferQueue.push(p);
    }
    m_deferCv.notify_one();
}

// ---------------------------------------------------------------------------
// stop — signal the recycle worker to drain and exit
// ---------------------------------------------------------------------------

void UploadHeapBufferPool::stop() {
    if (!m_recycleWorker.joinable()) return;

    m_stopRequested.store(true, std::memory_order_release);
    m_deferCv.notify_all();
    m_recycleWorker.join();
}

// ---------------------------------------------------------------------------
// recycleWorkerFunc — background fence-wait + recycle loop
// ---------------------------------------------------------------------------

void UploadHeapBufferPool::recycleWorkerFunc() {
    tracy::SetThreadName("UploadHeapPoolRecycle");

    while (true) {
        UploadHeapBuffer* p = nullptr;

        {
            std::unique_lock<std::mutex> lk(m_deferMutex);
            m_deferCv.wait(lk, [this] {
                return !m_deferQueue.empty() || m_stopRequested.load(std::memory_order_acquire);
            });

            if (m_deferQueue.empty()) {
                // m_stopRequested is set and queue is drained — exit.
                break;
            }

            p = m_deferQueue.front();
            m_deferQueue.pop();
        }

        if (!p) continue;

        // Fence-wait. The buffer was placed here because the GPU hadn't
        // finished reading it at drop time. We do the blocking wait here,
        // off the show thread.
        if (m_fence && p->lastCopyFenceValue > 0) {
            const uint64_t completed = m_fence->GetCompletedValue();

            if (completed == UINT64_MAX) {
                // Device removed — destroy without recycling.
                std::cerr << "[UploadHeapBufferPool] Recycle worker: device removed "
                             "(UINT64_MAX); destroying deferred buffer.\n";
                delete p;
                continue;
            }

            if (completed < p->lastCopyFenceValue) {
                // Still waiting. Use the per-buffer event (auto-reset).
                HRESULT hr = m_fence->SetEventOnCompletion(p->lastCopyFenceValue,
                                                           p->fenceEvent);
                if (SUCCEEDED(hr)) {
                    // 5 s cap matches the D3D12Renderer device-loss timeout.
                    const DWORD waitResult = WaitForSingleObject(p->fenceEvent, 5000);

                    // ResetEvent unconditionally before branching on the result.
                    // If the wait timed out, the kernel registration for
                    // lastCopyFenceValue is still pending — when the GPU
                    // eventually reaches that value it WILL fire the HANDLE.
                    // Without ResetEvent, a recycled buffer's next
                    // SetEventOnCompletion wait could unblock prematurely on
                    // the stale signal from the old registration, causing a
                    // GPU/CPU race on buffer content. Auto-reset is idempotent
                    // on an already-reset event, so this is safe on success too.
                    ResetEvent(p->fenceEvent);

                    if (waitResult != WAIT_OBJECT_0) {
                        // WAIT_TIMEOUT or WAIT_FAILED: the GPU has demonstrably
                        // not finished with this buffer. Destroy rather than
                        // recycle — treat as TDR-adjacent and don't risk a race.
                        std::cerr << "[UploadHeapBufferPool] Recycle worker: "
                                     "fence wait timeout/failure (result=0x"
                                  << std::hex << waitResult << std::dec
                                  << ") for buffer; destroying instead of recycling.\n";
                        delete p;
                        continue;
                    }
                } else {
                    std::cerr << "[UploadHeapBufferPool] Recycle worker: "
                                 "SetEventOnCompletion HRESULT 0x"
                              << std::hex << hr << std::dec
                              << " — destroying buffer (fence wait skipped)\n";
                    delete p;
                    continue;
                }
            }
        }

        // Return to the pool (or destroy if stop was requested — GPU is
        // presumed idle at process teardown and recycling to an emptying pool
        // is pointless; just free the resource).
        if (!m_stopRequested.load(std::memory_order_acquire)) {
            release(std::unique_ptr<UploadHeapBuffer>(p));
        } else {
            delete p;
        }
    }
}

} // namespace entity
