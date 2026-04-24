#include "entity/media/HapFormat.hpp"

#include <snappy.h>
#include <cstring>
#include <iostream>

namespace entity {

namespace {

// Section type bytes from the HAP spec. Layout:
//   high nibble = compression (A=None, B=Snappy, C=Chunked)
//   low nibble  = variant     (1=Hap, 5=Hap Alpha, Y=0xF=HapY/Q, 7=Hap R,
//                              1=HapA (single-A), 2/3=HapH unsigned/signed)
//
// The spec calls these "Section Types" for top-level frame sections.
enum SectionType : uint8_t {
    // Hap1 / Hap (RGB DXT1 / BC1)
    SEC_HAP_RAW        = 0xAB,
    SEC_HAP_SNAPPY     = 0xBB,
    SEC_HAP_CHUNKED    = 0xCB,

    // Hap5 / Hap Alpha (RGBA DXT5 / BC3)
    SEC_HAP5_RAW       = 0xAE,
    SEC_HAP5_SNAPPY    = 0xBE,
    SEC_HAP5_CHUNKED   = 0xCE,

    // HapY / Hap Q (Scaled YCoCg DXT5 / BC3)
    SEC_HAPY_RAW       = 0xAF,
    SEC_HAPY_SNAPPY    = 0xBF,
    SEC_HAPY_CHUNKED   = 0xCF,

    // Hap7 / Hap R (RGBA BC7) — the newer, higher-quality variant
    SEC_HAP7_RAW       = 0xAC,
    SEC_HAP7_SNAPPY    = 0xBC,
    SEC_HAP7_CHUNKED   = 0xCC,

    // HapA / Hap Alpha-Only (RGTC1 / BC4)
    SEC_HAPA_RAW       = 0xA1,
    SEC_HAPA_SNAPPY    = 0xB1,
    SEC_HAPA_CHUNKED   = 0xC1,

    // HapH unsigned HDR (BC6U)
    SEC_HAPHU_RAW      = 0xA2,
    SEC_HAPHU_SNAPPY   = 0xB2,
    SEC_HAPHU_CHUNKED  = 0xC2,

    // HapH signed HDR (BC6S)
    SEC_HAPHS_RAW      = 0xA3,
    SEC_HAPHS_SNAPPY   = 0xB3,
    SEC_HAPHS_CHUNKED  = 0xC3,

    // HapM multi-image container (YCoCg + separate Alpha)
    SEC_HAPM_MULTI     = 0x0D,

    // Subsections used inside the Chunked (0xC_) payload:
    SUB_DECODE_INSTRUCTIONS   = 0x01, // container holding the three tables below
    SUB_CHUNK_COMPRESSOR_TABLE = 0x02, // one byte per chunk (0x0A raw, 0x0B snappy)
    SUB_CHUNK_SIZE_TABLE       = 0x03, // 4 bytes LE per chunk
    SUB_CHUNK_OFFSET_TABLE     = 0x04, // optional, 4 bytes LE per chunk
};

// Per-chunk compression byte (inside Chunked payloads)
constexpr uint8_t CHUNK_COMPRESSOR_NONE   = 0x0A;
constexpr uint8_t CHUNK_COMPRESSOR_SNAPPY = 0x0B;

struct SectionHeader {
    uint32_t size{0};    // size of the section payload (excluding header)
    uint8_t  type{0};    // type byte (byte 3 in both 4- and 8-byte header layouts)
    uint32_t headerLen{0}; // 4 or 8
};

/**
 * Read a section header from `data`. Returns false if `data` is too short
 * to contain a valid header, or if a claimed 32-bit size doesn't fit the
 * remaining buffer.
 *
 * Header format per HAP spec:
 *   - 4-byte header: bytes 0-2 are LE uint24 size; byte 3 is type
 *   - 8-byte header: bytes 0-2 are all 0x00 (signal); byte 3 is type;
 *                    bytes 4-7 are LE uint32 size
 */
bool readSectionHeader(const uint8_t* data, size_t avail, SectionHeader& out) {
    if (avail < 4) return false;
    const uint32_t size24 = static_cast<uint32_t>(data[0])
                          | (static_cast<uint32_t>(data[1]) << 8)
                          | (static_cast<uint32_t>(data[2]) << 16);
    out.type = data[3];
    if (size24 == 0) {
        // 8-byte header — 32-bit size in bytes 4..7
        if (avail < 8) return false;
        out.size = static_cast<uint32_t>(data[4])
                 | (static_cast<uint32_t>(data[5]) << 8)
                 | (static_cast<uint32_t>(data[6]) << 16)
                 | (static_cast<uint32_t>(data[7]) << 24);
        out.headerLen = 8;
    } else {
        out.size = size24;
        out.headerLen = 4;
    }
    if (static_cast<size_t>(out.headerLen) + static_cast<size_t>(out.size) > avail) {
        return false;
    }
    return true;
}

/**
 * Map a top-level section type to its pixel format + color-space metadata.
 * Returns false if the type isn't a recognized single-image section.
 */
bool describeTopLevelSection(uint8_t type, HapPixelFormat& fmt, HapColorSpace& cs) {
    switch (type) {
        case SEC_HAP_RAW: case SEC_HAP_SNAPPY: case SEC_HAP_CHUNKED:
            fmt = HapPixelFormat::BC1_UNORM; cs = HapColorSpace::RGB; return true;
        case SEC_HAP5_RAW: case SEC_HAP5_SNAPPY: case SEC_HAP5_CHUNKED:
            fmt = HapPixelFormat::BC3_UNORM; cs = HapColorSpace::RGB; return true;
        case SEC_HAPY_RAW: case SEC_HAPY_SNAPPY: case SEC_HAPY_CHUNKED:
            fmt = HapPixelFormat::BC3_UNORM; cs = HapColorSpace::YCoCg_scaled; return true;
        case SEC_HAP7_RAW: case SEC_HAP7_SNAPPY: case SEC_HAP7_CHUNKED:
            fmt = HapPixelFormat::BC7_UNORM; cs = HapColorSpace::RGB; return true;
        case SEC_HAPA_RAW: case SEC_HAPA_SNAPPY: case SEC_HAPA_CHUNKED:
            fmt = HapPixelFormat::BC4_UNORM; cs = HapColorSpace::Alpha; return true;
        case SEC_HAPHU_RAW: case SEC_HAPHU_SNAPPY: case SEC_HAPHU_CHUNKED:
            fmt = HapPixelFormat::BC6H_UF16; cs = HapColorSpace::HDR_float; return true;
        case SEC_HAPHS_RAW: case SEC_HAPHS_SNAPPY: case SEC_HAPHS_CHUNKED:
            fmt = HapPixelFormat::BC6H_SF16; cs = HapColorSpace::HDR_float; return true;
        default:
            return false;
    }
}

// High nibble determines compression path.
enum class Compression { Raw, Snappy, Chunked };
Compression compressionFromType(uint8_t type) {
    const uint8_t high = type & 0xF0;
    if (high == 0xA0 || type == SEC_HAPA_RAW)    return Compression::Raw;
    if (high == 0xB0 || type == SEC_HAPA_SNAPPY) return Compression::Snappy;
    if (high == 0xC0 || type == SEC_HAPA_CHUNKED) return Compression::Chunked;
    // Note: SEC_HAPA_* (0xA1/0xB1/0xC1) happen to match the high-nibble pattern
    // too — the explicit checks above are defensive redundancy, not required.
    return Compression::Raw; // unreachable for valid inputs
}

/**
 * Decompress a single Snappy-compressed block appended into `out`.
 * Returns false on snappy errors.
 */
bool snappyInflateAppend(const uint8_t* src, size_t srcSize, std::vector<uint8_t>& out) {
    size_t uncompressedSize = 0;
    if (!snappy::GetUncompressedLength(reinterpret_cast<const char*>(src),
                                       srcSize, &uncompressedSize)) {
        std::cerr << "HapFormat: snappy::GetUncompressedLength failed" << std::endl;
        return false;
    }
    const size_t prevSize = out.size();
    out.resize(prevSize + uncompressedSize);
    if (!snappy::RawUncompress(reinterpret_cast<const char*>(src), srcSize,
                               reinterpret_cast<char*>(out.data() + prevSize))) {
        std::cerr << "HapFormat: snappy::RawUncompress failed" << std::endl;
        out.resize(prevSize);
        return false;
    }
    return true;
}

/**
 * Parse a Chunked (0xC_) payload. The payload is a single 0x01
 * "Decode Instructions" section containing three tables:
 *   - 0x02 ChunkCompressorTable:  1 byte per chunk (0x0A raw | 0x0B snappy)
 *   - 0x03 ChunkSizeTable:        4 bytes LE per chunk (compressed size)
 *   - 0x04 ChunkOffsetTable:      4 bytes LE per chunk (optional; if absent,
 *                                 chunks are laid out end-to-end after the
 *                                 Decode Instructions section in the order
 *                                 given by the compressor table)
 *
 * Chunks can be decompressed in parallel in principle; we do them serially
 * here for simplicity (snappy is fast and most real content uses Snappy
 * not Chunked; this is a cold path).
 */
bool parseChunkedPayload(const uint8_t* payload, size_t payloadSize,
                        std::vector<uint8_t>& out) {
    out.clear();

    // Expect the payload to begin with a 0x01 Decode Instructions section
    SectionHeader di;
    if (!readSectionHeader(payload, payloadSize, di) || di.type != SUB_DECODE_INSTRUCTIONS) {
        std::cerr << "HapFormat: chunked payload missing DecodeInstructions (0x01)" << std::endl;
        return false;
    }

    const uint8_t* cursor = payload + di.headerLen;
    const uint8_t* diEnd  = cursor + di.size;

    const uint8_t* compressorTable = nullptr;
    const uint8_t* sizeTable       = nullptr;
    const uint8_t* offsetTable     = nullptr;
    uint32_t chunkCount = 0;

    // Walk subsections inside DecodeInstructions, collecting table pointers.
    while (cursor < diEnd) {
        SectionHeader sub;
        if (!readSectionHeader(cursor, static_cast<size_t>(diEnd - cursor), sub)) {
            std::cerr << "HapFormat: malformed subsection inside DecodeInstructions" << std::endl;
            return false;
        }
        const uint8_t* subData = cursor + sub.headerLen;
        switch (sub.type) {
            case SUB_CHUNK_COMPRESSOR_TABLE:
                compressorTable = subData;
                chunkCount = sub.size; // one byte per chunk
                break;
            case SUB_CHUNK_SIZE_TABLE:
                sizeTable = subData;
                break;
            case SUB_CHUNK_OFFSET_TABLE:
                offsetTable = subData;
                break;
            default:
                // Unknown subsection — skip per spec's forward-compat rule.
                break;
        }
        cursor += sub.headerLen + sub.size;
    }

    if (!compressorTable || !sizeTable || chunkCount == 0) {
        std::cerr << "HapFormat: chunked payload missing required tables" << std::endl;
        return false;
    }

    // Chunks live in the bytes after the DecodeInstructions section.
    const uint8_t* chunksBase = payload + di.headerLen + di.size;
    const size_t chunksAvail = payloadSize - (di.headerLen + di.size);

    // Decode each chunk and append to out.
    uint32_t impliedOffset = 0;
    for (uint32_t i = 0; i < chunkCount; ++i) {
        const uint8_t compressor = compressorTable[i];
        const uint32_t chunkSize =
              static_cast<uint32_t>(sizeTable[i * 4 + 0])
            | (static_cast<uint32_t>(sizeTable[i * 4 + 1]) << 8)
            | (static_cast<uint32_t>(sizeTable[i * 4 + 2]) << 16)
            | (static_cast<uint32_t>(sizeTable[i * 4 + 3]) << 24);

        uint32_t chunkOffset;
        if (offsetTable) {
            chunkOffset = static_cast<uint32_t>(offsetTable[i * 4 + 0])
                        | (static_cast<uint32_t>(offsetTable[i * 4 + 1]) << 8)
                        | (static_cast<uint32_t>(offsetTable[i * 4 + 2]) << 16)
                        | (static_cast<uint32_t>(offsetTable[i * 4 + 3]) << 24);
        } else {
            chunkOffset = impliedOffset;
            impliedOffset += chunkSize;
        }

        if (static_cast<size_t>(chunkOffset) + chunkSize > chunksAvail) {
            std::cerr << "HapFormat: chunk " << i << " out of bounds" << std::endl;
            return false;
        }
        const uint8_t* chunkData = chunksBase + chunkOffset;

        if (compressor == CHUNK_COMPRESSOR_NONE) {
            out.insert(out.end(), chunkData, chunkData + chunkSize);
        } else if (compressor == CHUNK_COMPRESSOR_SNAPPY) {
            if (!snappyInflateAppend(chunkData, chunkSize, out)) {
                return false;
            }
        } else {
            std::cerr << "HapFormat: unknown chunk compressor 0x"
                      << std::hex << static_cast<int>(compressor) << std::dec << std::endl;
            return false;
        }
    }

    return true;
}

/**
 * Parse one top-level section into `outFormat`/`outColor`/`outBlocks`.
 * Handles Raw (0xA_), Snappy (0xB_), and Chunked (0xC_) compressions.
 * Not for HapM multi-image — that's peeled off by the caller.
 */
bool parseSingleImageSection(const uint8_t* section, size_t sectionSize,
                             HapPixelFormat& outFormat, HapColorSpace& outColor,
                             std::vector<uint8_t>& outBlocks) {
    SectionHeader hdr;
    if (!readSectionHeader(section, sectionSize, hdr)) {
        std::cerr << "HapFormat: top-level section header malformed" << std::endl;
        return false;
    }
    if (!describeTopLevelSection(hdr.type, outFormat, outColor)) {
        std::cerr << "HapFormat: unrecognized top-level section type 0x"
                  << std::hex << static_cast<int>(hdr.type) << std::dec << std::endl;
        return false;
    }

    const uint8_t* payload = section + hdr.headerLen;
    const size_t payloadSize = hdr.size;

    outBlocks.clear();
    switch (compressionFromType(hdr.type)) {
        case Compression::Raw:
            outBlocks.assign(payload, payload + payloadSize);
            return true;
        case Compression::Snappy:
            return snappyInflateAppend(payload, payloadSize, outBlocks);
        case Compression::Chunked:
            return parseChunkedPayload(payload, payloadSize, outBlocks);
    }
    return false;
}

} // anonymous namespace

bool parseHapPacket(const uint8_t* packetData, size_t packetSize,
                    uint32_t width, uint32_t height, HapFrame& out) {
    out.format = HapPixelFormat::Unknown;
    out.alphaFormat = HapPixelFormat::Unknown;
    out.width = width;
    out.height = height;
    out.textureData.clear();
    out.alphaData.clear();

    if (!packetData || packetSize < 4) {
        return false;
    }

    // Peek at the outer section to see if this is HapM (multi-image).
    SectionHeader outer;
    if (!readSectionHeader(packetData, packetSize, outer)) {
        return false;
    }

    if (outer.type == SEC_HAPM_MULTI) {
        // HapM wraps two sub-images (typically YCoCg color + BC4 alpha).
        const uint8_t* inner = packetData + outer.headerLen;
        const uint8_t* innerEnd = inner + outer.size;

        bool gotColor = false, gotAlpha = false;
        while (inner < innerEnd) {
            SectionHeader sub;
            if (!readSectionHeader(inner, static_cast<size_t>(innerEnd - inner), sub)) {
                return false;
            }
            const size_t subTotal = sub.headerLen + sub.size;

            HapPixelFormat fmt;
            HapColorSpace cs;
            if (!describeTopLevelSection(sub.type, fmt, cs)) {
                std::cerr << "HapFormat: HapM contains unknown section type 0x"
                          << std::hex << static_cast<int>(sub.type) << std::dec << std::endl;
                return false;
            }

            std::vector<uint8_t>& target = (cs == HapColorSpace::Alpha) ? out.alphaData
                                                                       : out.textureData;
            HapPixelFormat& targetFmt    = (cs == HapColorSpace::Alpha) ? out.alphaFormat
                                                                       : out.format;
            HapColorSpace& targetCS      = out.colorSpace; // primary plane's color space
            (void)targetCS;

            HapPixelFormat parsedFmt;
            HapColorSpace parsedCS;
            if (!parseSingleImageSection(inner, static_cast<size_t>(innerEnd - inner),
                                         parsedFmt, parsedCS, target)) {
                return false;
            }
            targetFmt = parsedFmt;
            if (cs != HapColorSpace::Alpha) {
                out.colorSpace = parsedCS; // primary plane drives the composite color-space
                gotColor = true;
            } else {
                gotAlpha = true;
            }

            inner += subTotal;
        }

        if (!gotColor) {
            std::cerr << "HapFormat: HapM frame without primary (color) plane" << std::endl;
            return false;
        }
        (void)gotAlpha;
        return true;
    }

    // Single-image frame — the top-level section IS the image.
    return parseSingleImageSection(packetData, packetSize,
                                   out.format, out.colorSpace, out.textureData);
}

} // namespace entity
