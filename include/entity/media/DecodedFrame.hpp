#pragma once

#include "entity/core/Types.hpp"

#include <atomic>
#include <cstdint>
#include <vector>

namespace entity {

/**
 * DecodedFrame — single decoded video frame with metadata.
 *
 * Lives in this header (rather than FrameRingBuffer.hpp where it used to)
 * so consumers can include it without dragging in the now-deprecated ring
 * buffer. The cache (FrameCache.hpp) holds these via shared_ptr; decoders
 * write into a freshly-allocated one each tick.
 *
 * `data` is either RGBA8 pixels OR a pre-compressed BCn block payload —
 * `format` says which. `valid` is atomic to support the older ring-buffer
 * read pattern; the cache treats entries as immutable so the field is
 * effectively a constant `true` once a frame is in the cache.
 */
struct DecodedFrame {
    std::vector<uint8_t> data;                       // RGBA pixels OR BC blocks, see `format`
    FrameNumber          frameNumber{-1};            // Source frame index
    uint32_t             width{0};
    uint32_t             height{0};
    Timestamp            pts{0};                     // Presentation timestamp (microseconds)
    TextureFormat        format{TextureFormat::RGBA8_UNORM};
    TextureColorSpace    colorSpace{TextureColorSpace::Linear}; // HAP Q → YCoCg_scaled
    std::atomic<bool>    valid{false};               // Holds-meaningful-data flag

    DecodedFrame() = default;

    DecodedFrame(const DecodedFrame& other)
        : data(other.data)
        , frameNumber(other.frameNumber)
        , width(other.width)
        , height(other.height)
        , pts(other.pts)
        , format(other.format)
        , colorSpace(other.colorSpace)
        , valid(other.valid.load(std::memory_order_acquire))
    {}

    DecodedFrame& operator=(const DecodedFrame& other) {
        if (this != &other) {
            data = other.data;
            frameNumber = other.frameNumber;
            width = other.width;
            height = other.height;
            pts = other.pts;
            format = other.format;
            colorSpace = other.colorSpace;
            valid.store(other.valid.load(std::memory_order_acquire), std::memory_order_release);
        }
        return *this;
    }

    DecodedFrame(DecodedFrame&& other) noexcept
        : data(std::move(other.data))
        , frameNumber(other.frameNumber)
        , width(other.width)
        , height(other.height)
        , pts(other.pts)
        , format(other.format)
        , colorSpace(other.colorSpace)
        , valid(other.valid.load(std::memory_order_acquire))
    {}

    DecodedFrame& operator=(DecodedFrame&& other) noexcept {
        if (this != &other) {
            data = std::move(other.data);
            frameNumber = other.frameNumber;
            width = other.width;
            height = other.height;
            pts = other.pts;
            format = other.format;
            colorSpace = other.colorSpace;
            valid.store(other.valid.load(std::memory_order_acquire), std::memory_order_release);
        }
        return *this;
    }

    /**
     * Bytes required for a frame of (w, h) in the given format.
     * RGBA8 → w*h*4. BC formats use 4×4 blocks (8 or 16 bytes per block).
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

    void allocate(uint32_t w, uint32_t h, TextureFormat fmt = TextureFormat::RGBA8_UNORM) {
        width = w;
        height = h;
        format = fmt;
        data.resize(bytesFor(w, h, fmt));
    }

    void clear() {
        data.clear();
        frameNumber = -1;
        width = 0;
        height = 0;
        pts = 0;
        format = TextureFormat::RGBA8_UNORM;
        colorSpace = TextureColorSpace::Linear;
        valid.store(false, std::memory_order_release);
    }
};

} // namespace entity
