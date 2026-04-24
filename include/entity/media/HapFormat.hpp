#pragma once

/**
 * HapFormat — parser for the HAP video codec's frame payload format.
 *
 * HAP is a GPU-friendly codec: each frame is a blob of pre-compressed BC
 * texture blocks (optionally wrapped in Snappy compression), which decode
 * on the GPU natively as BC{1,3,4,7} or BC6H textures. CPU cost is near-zero
 * vs. ProRes / H264 — there's no YUV→RGB conversion, no entropy decode,
 * just snappy decompression (if any) and a block-level upload.
 *
 * This parser takes a raw packet as demuxed from the container (MOV/AVI)
 * and produces a HapFrame describing the texture format + block data ready
 * for direct GPU upload. It handles every published HAP variant:
 *   - Hap1 / Hap (BC1 RGB)
 *   - Hap5 / Hap Alpha (BC3 RGBA)
 *   - HapY / Hap Q (BC3 carrying scaled YCoCg; shader converts)
 *   - HapA / Hap Alpha-Only (BC4 single-channel alpha)
 *   - Hap7 / Hap R (BC7 RGBA — the newer, higher-quality variant)
 *   - HapH / Hap HDR (BC6H-UF16 or BC6H-SF16 float-RGB)
 *   - HapM multi-image (YCoCg + separate Alpha plane)
 *
 * Spec reference:
 *   https://github.com/Vidvox/hap/blob/master/documentation/HapVideoDRAFT.md
 */

#include "entity/core/Types.hpp"
#include <cstdint>
#include <vector>

namespace entity {

/**
 * Compressed texture format of the HAP payload. Maps 1:1 to a DXGI_FORMAT
 * at the D3D12 boundary (BC1_UNORM, BC3_UNORM, BC4_UNORM, BC6H_UF16/SF16,
 * BC7_UNORM).
 */
enum class HapPixelFormat : uint8_t {
    Unknown,
    BC1_UNORM,      // Hap (RGB, 8 bytes per 4x4 block)
    BC3_UNORM,      // Hap Alpha or Hap Q (16 bytes per 4x4 block)
    BC4_UNORM,      // Hap Alpha-Only (8 bytes per 4x4 block, single channel)
    BC6H_UF16,      // Hap HDR unsigned float (16 bytes per 4x4 block)
    BC6H_SF16,      // Hap HDR signed float (16 bytes per 4x4 block)
    BC7_UNORM,      // Hap R (16 bytes per 4x4 block, highest quality)
};

/**
 * Sampler-side interpretation of the decoded texture. The compositor's
 * pixel shader branches on this to do the right color-space dance.
 */
enum class HapColorSpace : uint8_t {
    RGB,            // Direct RGB or RGBA (Hap, Hap Alpha, Hap R)
    YCoCg_scaled,   // Hap Q — shader converts scaled YCoCg → RGB
    Alpha,          // Single-channel alpha (Hap A)
    HDR_float,      // Hap H — BC6H HDR float RGB
};

/**
 * Parsed HAP frame ready for GPU upload. For the HapM multi-image case
 * (YCoCg + separate Alpha plane), `alphaFormat` != Unknown and `alphaData`
 * is populated; callers should create a second texture from it.
 */
struct HapFrame {
    HapPixelFormat format{HapPixelFormat::Unknown};
    HapColorSpace colorSpace{HapColorSpace::RGB};
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> textureData;  // Densely-packed BC blocks

    // HapM second plane (YCoCg + Alpha). Unknown format = single-plane frame.
    HapPixelFormat alphaFormat{HapPixelFormat::Unknown};
    std::vector<uint8_t> alphaData;
};

/**
 * Parse a raw HAP packet (as demuxed from the container) into a HapFrame.
 *
 * `out`'s buffers are reused if already sized correctly — avoids per-frame
 * allocations at steady state.
 *
 * `width` / `height` come from the container's AVStream codecpar (HAP
 * packets don't carry dimensions; the container does).
 *
 * Returns true on success. On malformed input, returns false and leaves
 * `out` in an unspecified state (caller should treat as decode failure).
 */
bool parseHapPacket(const uint8_t* packetData, size_t packetSize,
                    uint32_t width, uint32_t height, HapFrame& out);

/**
 * Expected BC-block byte size per 4x4 pixel region for each format.
 * Used to size texture buffers and validate decoded sizes.
 */
constexpr uint32_t bytesPerBlock(HapPixelFormat fmt) {
    switch (fmt) {
        case HapPixelFormat::BC1_UNORM:  return 8;
        case HapPixelFormat::BC4_UNORM:  return 8;
        case HapPixelFormat::BC3_UNORM:  return 16;
        case HapPixelFormat::BC6H_UF16:  return 16;
        case HapPixelFormat::BC6H_SF16:  return 16;
        case HapPixelFormat::BC7_UNORM:  return 16;
        default:                         return 0;
    }
}

/**
 * Total decompressed size in bytes for a frame of the given format + dims.
 * Accounts for 4x4 block padding at non-multiple-of-4 edges (HAP always
 * pads up; the compositor sees the full padded texture).
 */
constexpr size_t expectedTextureBytes(HapPixelFormat fmt, uint32_t w, uint32_t h) {
    const uint32_t blocksX = (w + 3) / 4;
    const uint32_t blocksY = (h + 3) / 4;
    return static_cast<size_t>(blocksX) * blocksY * bytesPerBlock(fmt);
}

} // namespace entity
