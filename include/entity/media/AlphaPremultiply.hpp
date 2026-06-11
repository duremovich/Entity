#pragma once
// SPDX-License-Identifier: GPL-3.0-or-later WITH Plugin-Linking-Exception
// (see LICENSE)
//
// Fused alpha-premultiply + row copy for RGBA8 decode output.
//
// Extracted from ProResDecoder::convertToRGBA so the premultiply math can be
// unit-tested on synthetic buffers (tests/unit/AlphaPremultiplyTests.cpp)
// without a full FFmpeg / D3D12 stack.
//
// Performance note: in production the destination is a persistently-mapped
// D3D12 UPLOAD-heap pointer (WRITE_COMBINE memory). CPU reads from WC are
// ~100x slower than cached RAM. This function therefore reads ONLY from `src`
// (which the caller keeps in normal cached memory) and WRITES ONLY to `dst` —
// `dst` is never read back. See the loop body: every `dst` byte is assigned,
// none is read.

#include <cstdint>
#include <cstddef>

namespace entity::media {

// Premultiply RGBA8 by its own alpha, reading from `src` and writing to `dst`.
//
// Layout: both buffers are RGBA8 (4 bytes/pixel), `width` pixels per row,
// `height` rows. `srcPitch` / `dstPitch` are the byte stride between rows
// (>= width*4); they may differ (e.g. cached scratch is tight-packed while the
// upload-heap destination uses a 256-byte-aligned D3D12 footprint pitch).
//
// `dst` may be WRITE_COMBINE upload-heap memory — write-only, never read.
//
// Division-by-255 with round-to-nearest via the classic fast approximation:
//   (c*a + 128 + ((c*a + 128) >> 8)) >> 8
// Exact for a == 255 (identity) and a == 0 (zero). Branchless so the compiler
// can auto-vectorize (AVX2) the inner per-row loop.
inline void premultiplyRgbaRows(const uint8_t* src, size_t srcPitch,
                                uint8_t* dst, size_t dstPitch,
                                uint32_t width, uint32_t height) {
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* in = src + static_cast<size_t>(row) * srcPitch;
        uint8_t* out      = dst + static_cast<size_t>(row) * dstPitch;
        for (uint32_t col = 0; col < width; ++col) {
            const size_t off = static_cast<size_t>(col) * 4;
            const uint32_t a = in[off + 3];
            uint32_t t0 = in[off + 0] * a + 128;
            uint32_t t1 = in[off + 1] * a + 128;
            uint32_t t2 = in[off + 2] * a + 128;
            out[off + 0] = static_cast<uint8_t>((t0 + (t0 >> 8)) >> 8);
            out[off + 1] = static_cast<uint8_t>((t1 + (t1 >> 8)) >> 8);
            out[off + 2] = static_cast<uint8_t>((t2 + (t2 >> 8)) >> 8);
            out[off + 3] = static_cast<uint8_t>(a);
        }
    }
}

} // namespace entity::media
