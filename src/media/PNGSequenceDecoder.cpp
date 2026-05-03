#include "entity/media/PNGSequenceDecoder.hpp"
#include "entity/core/Settings.hpp"
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <cctype>
#include <cstring>
#include <fstream>

// stb_image single-header library for PNG decoding
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb_image.h"

namespace entity {

PNGSequenceDecoder::PNGSequenceDecoder() = default;

PNGSequenceDecoder::~PNGSequenceDecoder() {
    close();
}

Result PNGSequenceDecoder::open(const std::string& filepath) {
    // If already open, close first
    if (m_isOpen) {
        close();
    }

    m_basePath = filepath;

    // Scan directory for PNG files
    Result result = scanDirectory();
    if (result != Result::Success) {
        return result;
    }

    // Ensure we found PNG files
    if (m_fileList.empty()) {
        std::cerr << "PNGSequenceDecoder: No PNG files found in directory" << std::endl;
        return Result::FileNotFound;
    }

    // Load first frame to determine dimensions and alpha channel
    DecodedFrame firstFrame;
    result = loadPNG(m_fileList[0], firstFrame);
    if (result != Result::Success) {
        std::cerr << "PNGSequenceDecoder: Failed to load first frame for dimensions" << std::endl;
        return result;
    }

    // Store dimensions and properties
    m_width = firstFrame.width;
    m_height = firstFrame.height;
    m_duration = static_cast<FrameNumber>(m_fileList.size());
    m_isOpen = true;

    // Detect alpha channel from first frame data
    // If all alpha values are 255 (fully opaque), assume no meaningful alpha
    bool hasAlpha = false;
    if (!firstFrame.data.empty() && m_width > 0 && m_height > 0) {
        size_t pixelCount = static_cast<size_t>(m_width) * m_height;
        for (size_t i = 3; i < firstFrame.data.size(); i += 4) {
            if (firstFrame.data[i] != 255) {
                hasAlpha = true;
                break;
            }
        }
    }
    m_hasAlpha = hasAlpha;

    std::cout << "PNGSequenceDecoder: Opened sequence with " << m_fileList.size()
              << " frames (" << m_width << "x" << m_height
              << ", alpha=" << (m_hasAlpha ? "yes" : "no") << ")" << std::endl;

    return Result::Success;
}

void PNGSequenceDecoder::close() {
    m_fileList.clear();
    m_isOpen = false;
    m_width = 0;
    m_height = 0;
    m_duration = 0;
    m_currentFrame = -1;
}

Result PNGSequenceDecoder::decodeFrame(FrameNumber frameNumber, DecodedFrame& outFrame) {
    if (!m_isOpen) {
        return Result::Failure;
    }

    if (frameNumber < 0 || frameNumber >= m_duration) {
        return Result::InvalidParameter;
    }

    // Load PNG file at frame index
    size_t fileIndex = static_cast<size_t>(frameNumber);
    if (fileIndex >= m_fileList.size()) {
        return Result::InvalidParameter;
    }

    return loadPNG(m_fileList[fileIndex], outFrame);
}

Result PNGSequenceDecoder::seek(FrameNumber frameNumber) {
    if (!m_isOpen) {
        return Result::Failure;
    }

    if (frameNumber < 0 || frameNumber >= m_duration) {
        return Result::InvalidParameter;
    }

    m_currentFrame = frameNumber;
    return Result::Success;
}

Result PNGSequenceDecoder::scanDirectory() {
    // Numeric sort: compare the trailing number in each filename so that
    // frame_2.png < frame_10.png (alphabetical order gets this wrong).
    auto numericSort = [](const std::string& a, const std::string& b) {
        auto trailingNum = [](const std::string& path) -> long long {
            std::string stem = std::filesystem::path(path).stem().string();
            size_t i = stem.size();
            while (i > 0 && std::isdigit((unsigned char)stem[i - 1])) --i;
            if (i == stem.size()) return 0LL;
            try { return std::stoll(stem.substr(i)); } catch (...) { return 0LL; }
        };
        long long na = trailingNum(a), nb = trailingNum(b);
        return na != nb ? na < nb : a < b;
    };

    try {
        std::filesystem::path inputPath(m_basePath);

        // --- Directory input: include all PNGs (used when a folder is passed directly) ---
        if (std::filesystem::is_directory(inputPath)) {
            for (const auto& entry : std::filesystem::directory_iterator(inputPath)) {
                if (!entry.is_regular_file()) continue;
                std::string ext = entry.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (ext == ".png") m_fileList.push_back(entry.path().string());
            }
            std::sort(m_fileList.begin(), m_fileList.end(), numericSort);
            return Result::Success;
        }

        // --- File input ---
        if (!std::filesystem::is_regular_file(inputPath)) {
            std::cerr << "PNGSequenceDecoder: File not found: " << m_basePath << std::endl;
            return Result::FileNotFound;
        }

        std::string stem = inputPath.stem().string();

        // Find the trailing digit run in the stem, if any.
        size_t numStart = stem.size();
        while (numStart > 0 && std::isdigit((unsigned char)stem[numStart - 1]))
            --numStart;

        if (numStart == stem.size()) {
            // No trailing digits → standalone single-frame PNG.
            m_fileList.push_back(inputPath.string());
            return Result::Success;
        }

        // Has trailing digits: base prefix + numeric suffix.
        // Collect all PNGs in the same directory that share the same prefix and
        // have an all-digit suffix (e.g. "frame_" + "042" + ".png").
        std::string base = stem.substr(0, numStart);
        std::filesystem::path dir = inputPath.parent_path();

        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file()) continue;
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (ext != ".png") continue;

            std::string candidateStem = entry.path().stem().string();
            if (candidateStem.size() <= base.size()) continue;
            if (candidateStem.substr(0, base.size()) != base) continue;

            // Everything after the base must be digits only
            std::string suffix = candidateStem.substr(base.size());
            if (suffix.empty()) continue;
            bool allDigits = std::all_of(suffix.begin(), suffix.end(),
                                         [](unsigned char c) { return std::isdigit(c); });
            if (!allDigits) continue;

            m_fileList.push_back(entry.path().string());
        }

        std::sort(m_fileList.begin(), m_fileList.end(), numericSort);
        return Result::Success;
    }
    catch (const std::exception& e) {
        std::cerr << "PNGSequenceDecoder: Error scanning directory: " << e.what() << std::endl;
        return Result::Failure;
    }
}

Result PNGSequenceDecoder::loadPNG(const std::string& filepath, DecodedFrame& outFrame) {
    // Read file into memory
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "PNGSequenceDecoder: Failed to open file: " << filepath << std::endl;
        return Result::FileNotFound;
    }

    // Get file size and validate
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Validate file size to prevent DoS attacks
    const size_t MAX_PNG_SIZE = 512 * 1024 * 1024;  // 512 MB max
    if (fileSize <= 0 || static_cast<size_t>(fileSize) > MAX_PNG_SIZE) {
        std::cerr << "PNGSequenceDecoder: File size invalid or too large: " << fileSize << " bytes (max: " << MAX_PNG_SIZE << ")" << std::endl;
        std::cerr << "  File: " << filepath << std::endl;
        return Result::OutOfMemory;
    }

    // Read file data
    std::vector<uint8_t> fileData(static_cast<size_t>(fileSize));
    if (!file.read(reinterpret_cast<char*>(fileData.data()), fileSize)) {
        std::cerr << "PNGSequenceDecoder: Failed to read file: " << filepath << std::endl;
        return Result::Failure;
    }
    file.close();

    // Decode PNG using stb_image
    int width = 0, height = 0, channels = 0;
    uint8_t* imageData = stbi_load_from_memory(
        fileData.data(),
        static_cast<int>(fileData.size()),
        &width,
        &height,
        &channels,
        4  // Force RGBA (4 channels)
    );

    if (!imageData) {
        std::cerr << "PNGSequenceDecoder: Failed to decode PNG: " << filepath << std::endl;
        std::cerr << "  stb_image error: " << stbi_failure_reason() << std::endl;
        return Result::DecoderError;
    }

    // Allocate output frame
    outFrame.allocate(static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    // Copy RGBA data and premultiply alpha
    size_t pixelDataSize = static_cast<size_t>(width) * height * 4;
    std::memcpy(outFrame.data.data(), imageData, pixelDataSize);
    premultiplyAlpha(outFrame.data.data(), static_cast<uint32_t>(width), static_cast<uint32_t>(height));

    // Free stb_image memory
    stbi_image_free(imageData);

    // Phase C.12 #6 — PNG payload is sRGB-encoded byte values per the PNG
    // spec; stb_image's stbi_load returns the raw bytes without
    // linearizing, so the post-decode color space is "sRGB - Display" in
    // the OCIO Studio Config (the display-referred sRGB encoding). The
    // OCIO input transform linearizes + promotes to ACEScg.
    //
    // C.12 #7 — user can override via Settings::defaultPngInputCs (e.g.
    // "Linear Rec.709 (sRGB)" if their pipeline pre-linearizes PNGs).
    // PNG has no per-file color-space metadata, so this default is the
    // only signal we have.
    outFrame.ocioColorSpace = activeSettings().defaultPngInputCs;

    // Mark frame as valid
    outFrame.valid.store(true, std::memory_order_release);

    return Result::Success;
}

void PNGSequenceDecoder::premultiplyAlpha(uint8_t* rgba, uint32_t width, uint32_t height) {
    if (!rgba || width == 0 || height == 0) {
        return;
    }

    size_t pixelCount = static_cast<size_t>(width) * height;
    for (size_t i = 0; i < pixelCount; ++i) {
        size_t pixelOffset = i * 4;

        // Get RGBA components
        uint8_t r = rgba[pixelOffset];
        uint8_t g = rgba[pixelOffset + 1];
        uint8_t b = rgba[pixelOffset + 2];
        uint8_t a = rgba[pixelOffset + 3];

        // Premultiply by alpha: RGB * (A / 255) with rounding to nearest
        rgba[pixelOffset] = static_cast<uint8_t>((static_cast<uint32_t>(r) * a + 127) / 255);
        rgba[pixelOffset + 1] = static_cast<uint8_t>((static_cast<uint32_t>(g) * a + 127) / 255);
        rgba[pixelOffset + 2] = static_cast<uint8_t>((static_cast<uint32_t>(b) * a + 127) / 255);
        // Alpha channel unchanged
    }
}

} // namespace entity
