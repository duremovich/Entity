#include "entity/media/PNGSequenceDecoder.hpp"
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
    try {
        std::filesystem::path basePath(m_basePath);
        std::filesystem::path directory;

        // If basePath is a file, use its directory
        if (std::filesystem::is_regular_file(basePath)) {
            directory = basePath.parent_path();
        } else if (std::filesystem::is_directory(basePath)) {
            directory = basePath;
        } else {
            // Try to interpret as directory anyway
            directory = basePath;
        }

        // Ensure directory is valid
        if (directory.empty() || !std::filesystem::exists(directory)) {
            std::cerr << "PNGSequenceDecoder: Directory not found: " << directory.string() << std::endl;
            return Result::FileNotFound;
        }

        // Scan for PNG files
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_regular_file()) {
                continue;
            }

            std::string ext = entry.path().extension().string();

            // Convert extension to lowercase for case-insensitive comparison
            std::transform(ext.begin(), ext.end(), ext.begin(),
                         [](unsigned char c) { return std::tolower(c); });

            // Check if PNG file
            if (ext == ".png") {
                m_fileList.push_back(entry.path().string());
            }
        }

        // Sort files alphabetically
        // This assumes files are named in a way that alphabetical order matches sequence order
        // e.g., frame_001.png, frame_002.png, ..., frame_010.png
        std::sort(m_fileList.begin(), m_fileList.end());

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
