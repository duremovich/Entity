#pragma once

#include "entity/core/Types.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace entity {

// Forward-declared so DecodedFrame.hpp stays out of D3D12 headers — only
// UploadHeap frames carry a non-null uploadBuf; CpuHeap frames leave it null.
struct UploadHeapBuffer;

/**
 * DecodedFrame — single decoded video frame with metadata.
 *
 * Lives in this header (rather than FrameRingBuffer.hpp where it used to)
 * so consumers can include it without dragging in the now-deprecated ring
 * buffer. The cache (FrameCache.hpp) holds these via shared_ptr; decoders
 * write into a freshly-allocated one each tick.
 *
 * Pixel bytes are held in one of two storage modes:
 *
 *   CpuHeap   — `cpuData` (std::vector<uint8_t>) — the current default. The
 *               FrameCache's custom deleter recycles the vector into the
 *               DecodeBufferPool to avoid per-frame malloc.
 *
 *   UploadHeap — `uploadBuf` (shared_ptr<UploadHeapBuffer>) — a D3D12
 *               UPLOAD-heap persistently-mapped buffer owned by
 *               UploadHeapBufferPool. The GPU-fence-wait before recycle is
 *               handled by the UploadHeapBuffer shared_ptr deleter (Phase 1).
 *               The FrameCache deleter does NOT touch uploadBuf; it just lets
 *               the shared_ptr decrement naturally on `delete p`.
 *
 * Use data() / mutableData() / size() for all pixel access — they dispatch
 * to the active storage without the caller needing to know which mode is live.
 *
 * `format` says which pixel encoding the bytes hold (RGBA8 or BCn blocks).
 * `valid` is atomic to support the older ring-buffer read pattern; the cache
 * treats entries as immutable so the field is effectively a constant `true`
 * once a frame is in the cache.
 */
struct DecodedFrame {
    // Storage discriminator. CpuHeap is the current default; UploadHeap is
    // used once decoders are wired to UploadHeapBufferPool (Phase 3+).
    enum class Storage { CpuHeap, UploadHeap };

    Storage              storage{Storage::CpuHeap};
    std::vector<uint8_t> cpuData;                    // CpuHeap path: RGBA pixels OR BC blocks
    std::shared_ptr<UploadHeapBuffer> uploadBuf;     // UploadHeap path: mapped D3D12 buffer

    FrameNumber          frameNumber{-1};            // Source frame index
    uint32_t             width{0};
    uint32_t             height{0};
    Timestamp            pts{0};                     // Presentation timestamp (microseconds)
    TextureFormat        format{TextureFormat::RGBA8_UNORM};
    TextureColorSpace    colorSpace{TextureColorSpace::Linear}; // HAP Q → YCoCg_scaled
    // Phase C.12 #6 — OCIO color-space name for the input transform pipeline.
    // Populated by per-codec decoders (HAP RGB → "Linear Rec.709 (sRGB)",
    // ProRes via AVColorSpace lookup, PNG → "sRGB", HAP Q post-YCoCg →
    // "Linear Rec.709 (sRGB)", HAP Alpha-only → "Raw"). Renderer hands this
    // to OcioManager::buildInputProcessor at draw time. Empty = "no
    // transform" (rendered as-is, identity OCIO path).
    std::string          ocioColorSpace;
    std::atomic<bool>    valid{false};               // Holds-meaningful-data flag

    // -----------------------------------------------------------------------
    // Pixel-buffer accessors — always use these; never access cpuData /
    // uploadBuf directly in production code.
    // -----------------------------------------------------------------------

    // Read-only pointer to the start of pixel data.
    const uint8_t* data() const;

    // Writable pointer to the start of pixel data. Only valid when the frame
    // is being written by a decoder (before FrameCache::put). After put() the
    // shared_ptr<const DecodedFrame> lease makes the frame immutable — callers
    // should not call mutableData() on a cache lease.
    uint8_t* mutableData();

    // Byte count of the pixel buffer. Corresponds to bytesFor(width, height, format)
    // for CPU-heap frames; for upload-heap frames may be larger due to row-pitch
    // alignment. Callers that only need to know the logical frame size should use
    // bytesFor() instead.
    size_t size() const;

    // Returns the row pitch (bytes per row) of the upload-heap buffer, as
    // populated by UploadHeapBufferPool::acquireForTexture. The pitch is
    // 256-byte-aligned (D3D12_TEXTURE_DATA_PITCH_ALIGNMENT). Returns 0 for
    // CpuHeap frames (tight-packed; callers use width*bytesPerPixel directly).
    // Defined in DecodedFrame.cpp so this header stays D3D12-header-free.
    uint32_t uploadRowPitch() const;

    // -----------------------------------------------------------------------

    DecodedFrame() = default;

    DecodedFrame(const DecodedFrame& other)
        : storage(other.storage)
        , cpuData(other.cpuData)
        , uploadBuf(other.uploadBuf)
        , frameNumber(other.frameNumber)
        , width(other.width)
        , height(other.height)
        , pts(other.pts)
        , format(other.format)
        , colorSpace(other.colorSpace)
        , ocioColorSpace(other.ocioColorSpace)
        , valid(other.valid.load(std::memory_order_acquire))
    {}

    DecodedFrame& operator=(const DecodedFrame& other) {
        if (this != &other) {
            storage = other.storage;
            cpuData = other.cpuData;
            uploadBuf = other.uploadBuf;
            frameNumber = other.frameNumber;
            width = other.width;
            height = other.height;
            pts = other.pts;
            format = other.format;
            colorSpace = other.colorSpace;
            ocioColorSpace = other.ocioColorSpace;
            valid.store(other.valid.load(std::memory_order_acquire), std::memory_order_release);
        }
        return *this;
    }

    DecodedFrame(DecodedFrame&& other) noexcept
        : storage(other.storage)
        , cpuData(std::move(other.cpuData))
        , uploadBuf(std::move(other.uploadBuf))
        , frameNumber(other.frameNumber)
        , width(other.width)
        , height(other.height)
        , pts(other.pts)
        , format(other.format)
        , colorSpace(other.colorSpace)
        , ocioColorSpace(std::move(other.ocioColorSpace))
        , valid(other.valid.load(std::memory_order_acquire))
    {}

    DecodedFrame& operator=(DecodedFrame&& other) noexcept {
        if (this != &other) {
            storage = other.storage;
            cpuData = std::move(other.cpuData);
            uploadBuf = std::move(other.uploadBuf);
            frameNumber = other.frameNumber;
            width = other.width;
            height = other.height;
            pts = other.pts;
            format = other.format;
            colorSpace = other.colorSpace;
            ocioColorSpace = std::move(other.ocioColorSpace);
            valid.store(other.valid.load(std::memory_order_acquire), std::memory_order_release);
        }
        return *this;
    }

    /**
     * Bytes required for a frame of (w, h) in the given format.
     * RGBA8 → w*h*4. BC formats use 4×4 blocks (8 or 16 bytes per block).
     * Note: upload-heap frames may allocate more than this (pitch alignment);
     * use size() for the actual allocation size.
     */
    static size_t bytesFor(uint32_t w, uint32_t h, TextureFormat fmt) {
        switch (fmt) {
            case TextureFormat::RGBA8_UNORM:
                return static_cast<size_t>(w) * h * 4;
            case TextureFormat::BC1_UNORM:
            case TextureFormat::BC4_UNORM: {
                const size_t bx = (w + 3) / 4;
                const size_t by = (h + 3) / 4;
                return bx * by * 8;
            }
            case TextureFormat::BC3_UNORM:
            case TextureFormat::BC6H_UF16:
            case TextureFormat::BC6H_SF16:
            case TextureFormat::BC7_UNORM: {
                const size_t bx = (w + 3) / 4;
                const size_t by = (h + 3) / 4;
                return bx * by * 16;
            }
        }
        return 0;
    }

    // Allocate a CpuHeap frame. Always uses cpuData; storage set to CpuHeap.
    void allocate(uint32_t w, uint32_t h, TextureFormat fmt = TextureFormat::RGBA8_UNORM) {
        width = w;
        height = h;
        format = fmt;
        storage = Storage::CpuHeap;
        uploadBuf.reset();  // release any prior UploadHeap slot before switching paths
        cpuData.resize(bytesFor(w, h, fmt));
    }

    void clear() {
        storage = Storage::CpuHeap;
        cpuData.clear();
        uploadBuf.reset();
        frameNumber = -1;
        width = 0;
        height = 0;
        pts = 0;
        format = TextureFormat::RGBA8_UNORM;
        colorSpace = TextureColorSpace::Linear;
        ocioColorSpace.clear();
        valid.store(false, std::memory_order_release);
    }
};

} // namespace entity
