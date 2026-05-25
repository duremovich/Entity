#pragma once

/**
 * Concrete IDecodeBufferAllocator implementations.
 *
 * CpuHeapDecodeBufferAllocator  — CPU heap path (wraps DecodeBufferPool).
 * UploadHeapDecodeBufferAllocator — D3D12 UPLOAD heap path (wraps
 *     UploadHeapBufferPool). Falls back to CpuHeap if the pool returns nullptr
 *     (device loss, resource exhaustion).
 *
 * Both are header-only; they delegate entirely to pool APIs that are
 * already implemented in their respective .cpp files.
 */

#include "entity/media/IDecodeBufferAllocator.hpp"
#include "entity/media/DecodeBufferPool.hpp"
#include "entity/render/UploadHeapBufferPool.hpp"

#include <iostream>
#include <memory>

namespace entity {

// ---------------------------------------------------------------------------
// CpuHeapDecodeBufferAllocator
// ---------------------------------------------------------------------------

class CpuHeapDecodeBufferAllocator final : public IDecodeBufferAllocator {
public:
    // pool may be null; if so, falls back to plain resize (same pre-Phase-3
    // behaviour, useful for unit tests that don't wire a pool).
    explicit CpuHeapDecodeBufferAllocator(DecodeBufferPool* pool) noexcept
        : m_pool(pool) {}

    void prepareDecodeBuffer(DecodedFrame& frame,
                             uint32_t      width,
                             uint32_t      height,
                             TextureFormat format = TextureFormat::RGBA8_UNORM) override {
        if (frame.storage == DecodedFrame::Storage::UploadHeap) {
            // Transitioning from UploadHeap back to CpuHeap — release the
            // upload buffer first to avoid a pool slot leak (mirrors the
            // allocate() fix in DecodedFrame.hpp).
            frame.uploadBuf.reset();
        }
        frame.storage = DecodedFrame::Storage::CpuHeap;

        const size_t bytes = DecodedFrame::bytesFor(width, height, format);
        if (frame.cpuData.size() != bytes) {
            if (m_pool) {
                frame.cpuData = m_pool->acquire(bytes);
            } else {
                frame.cpuData.resize(bytes);
            }
        }
    }

private:
    DecodeBufferPool* m_pool{nullptr}; // non-owning
};

// ---------------------------------------------------------------------------
// UploadHeapDecodeBufferAllocator
// ---------------------------------------------------------------------------

class UploadHeapDecodeBufferAllocator final : public IDecodeBufferAllocator {
public:
    // pool must be non-null; the UploadHeapBufferPool must outlive this allocator.
    explicit UploadHeapDecodeBufferAllocator(
        std::shared_ptr<UploadHeapBufferPool> pool) noexcept
        : m_pool(std::move(pool)) {}

    void prepareDecodeBuffer(DecodedFrame& frame,
                             uint32_t      width,
                             uint32_t      height,
                             TextureFormat format = TextureFormat::RGBA8_UNORM) override {
        // If we already have a compatible upload buffer for the same dimensions
        // and format, reuse it — the footprint (including RowPitch) is unchanged.
        if (frame.storage == DecodedFrame::Storage::UploadHeap
            && frame.uploadBuf
            && frame.width  == width
            && frame.height == height
            && frame.format == format) {
            return;
        }

        // Release any prior backing (either UploadHeap slot or CpuHeap vector)
        // before acquiring a fresh upload buffer.
        frame.cpuData.clear();
        frame.uploadBuf.reset();

        if (m_pool) {
            // Convert TextureFormat → DXGI_FORMAT for GetCopyableFootprints.
            // This mirrors the table in TextureUploader.cpp; kept inline here
            // so DecodeBufferAllocators.hpp stays self-contained.
            DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
            switch (format) {
                case TextureFormat::RGBA8_UNORM: dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; break;
                case TextureFormat::BC1_UNORM:   dxgi = DXGI_FORMAT_BC1_UNORM;       break;
                case TextureFormat::BC3_UNORM:   dxgi = DXGI_FORMAT_BC3_UNORM;       break;
                case TextureFormat::BC4_UNORM:   dxgi = DXGI_FORMAT_BC4_UNORM;       break;
                case TextureFormat::BC6H_UF16:   dxgi = DXGI_FORMAT_BC6H_UF16;       break;
                case TextureFormat::BC6H_SF16:   dxgi = DXGI_FORMAT_BC6H_SF16;       break;
                case TextureFormat::BC7_UNORM:   dxgi = DXGI_FORMAT_BC7_UNORM;       break;
            }

            if (dxgi != DXGI_FORMAT_UNKNOWN) {
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
                frame.uploadBuf = m_pool->acquireForTexture(width, height, dxgi, &fp);
                // footprint is also stamped on the buffer by acquireForTexture;
                // the outFootprint out-param here is discarded — callers read
                // frame.uploadBuf->footprint.
            }
        }

        if (frame.uploadBuf) {
            frame.storage = DecodedFrame::Storage::UploadHeap;
        } else {
            // Pool returned nullptr (device loss, resource pressure, unknown format).
            // Fall back to CPU heap so the decode worker keeps producing frames.
            std::cerr << "[UploadHeapDecodeBufferAllocator] acquireForTexture failed for "
                      << width << "x" << height << " — falling back to CPU heap\n";
            frame.storage = DecodedFrame::Storage::CpuHeap;
            frame.cpuData.resize(DecodedFrame::bytesFor(width, height, format));
        }
    }

private:
    std::shared_ptr<UploadHeapBufferPool> m_pool;
};

} // namespace entity
