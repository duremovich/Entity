/**
 * PNG Sequence Decoder - Practical Usage Examples
 *
 * This file demonstrates how to use PNGSequenceDecoder in various scenarios.
 * It's not meant to be compiled as-is, but rather as reference documentation.
 */

#include "entity/media/PNGSequenceDecoder.hpp"
#include <iostream>
#include <memory>

namespace entity::examples {

// ============================================================================
// Example 1: Simple PNG Sequence Loading and Frame Access
// ============================================================================

void example_basic_usage() {
    // Create decoder instance
    PNGSequenceDecoder decoder;

    // Open a PNG sequence
    // The path can be any file in the sequence (e.g., "frames/image_001.png")
    // or the directory itself (e.g., "frames/")
    Result result = decoder.open("C:\\data\\sequences\\image_001.png");

    if (result == Result::Success) {
        // Sequence loaded successfully
        std::cout << "Sequence loaded: " << decoder.getDuration() << " frames" << std::endl;
        std::cout << "Resolution: " << decoder.getWidth() << "x"
                  << decoder.getHeight() << std::endl;
        std::cout << "Has Alpha: " << (decoder.hasAlpha() ? "yes" : "no") << std::endl;

        // Decode a specific frame
        DecodedFrame frame;
        Result decodeResult = decoder.decodeFrame(0, frame);

        if (decodeResult == Result::Success) {
            // Frame decoded successfully
            std::cout << "Frame 0 decoded: " << frame.width << "x" << frame.height << std::endl;
            std::cout << "Data size: " << frame.data.size() << " bytes" << std::endl;

            // Access pixel data (RGBA8, premultiplied alpha)
            uint8_t* pixelData = frame.data.data();
            uint32_t pixelCount = frame.width * frame.height;

            // Example: Read first pixel
            uint8_t r = pixelData[0];
            uint8_t g = pixelData[1];
            uint8_t b = pixelData[2];
            uint8_t a = pixelData[3];

            std::cout << "First pixel: RGBA(" << (int)r << ", " << (int)g
                      << ", " << (int)b << ", " << (int)a << ")" << std::endl;
        }
    }
    else {
        std::cerr << "Failed to open sequence: " << ResultToString(result) << std::endl;
    }

    // Cleanup (also called in destructor)
    decoder.close();
}

// ============================================================================
// Example 2: Sequential Frame Decoding
// ============================================================================

void example_sequential_decode() {
    PNGSequenceDecoder decoder;

    if (decoder.open("sequences/frame_%03d.png") != Result::Success) {
        std::cerr << "Failed to open sequence" << std::endl;
        return;
    }

    // Decode all frames sequentially
    FrameNumber totalFrames = decoder.getDuration();

    for (FrameNumber frameNum = 0; frameNum < totalFrames; ++frameNum) {
        DecodedFrame frame;
        Result result = decoder.decodeFrame(frameNum, frame);

        if (result == Result::Success) {
            std::cout << "Frame " << frameNum << ": OK" << std::endl;
            // Process frame data...
        }
        else {
            std::cerr << "Frame " << frameNum << ": FAILED" << std::endl;
        }
    }
}

// ============================================================================
// Example 3: Error Handling and Validation
// ============================================================================

void example_error_handling() {
    PNGSequenceDecoder decoder;

    // Attempt to open non-existent sequence
    Result result = decoder.open("C:\\nonexistent\\sequence\\image.png");

    switch (result) {
        case Result::Success:
            std::cout << "Sequence loaded successfully" << std::endl;
            break;

        case Result::FileNotFound:
            std::cerr << "Sequence not found at specified path" << std::endl;
            std::cerr << "Checked directory: " << decoder.getFilePath() << std::endl;
            break;

        case Result::DecoderError:
            std::cerr << "Failed to decode PNG file (corrupt or unsupported format)" << std::endl;
            break;

        case Result::InvalidParameter:
            std::cerr << "Invalid parameter passed to decoder" << std::endl;
            break;

        case Result::Failure:
            std::cerr << "Generic failure (decoder may not be open)" << std::endl;
            break;

        default:
            std::cerr << "Unknown error: " << ResultToString(result) << std::endl;
            break;
    }
}

// ============================================================================
// Example 4: Using Factory Function for Decoder Creation
// ============================================================================

void example_factory_pattern() {
    // Use the factory function to create appropriate decoder
    std::unique_ptr<Decoder> decoder = createDecoder(MediaType::PNGSequence);

    if (!decoder) {
        std::cerr << "Failed to create decoder" << std::endl;
        return;
    }

    // Decoder is generic pointer, but we know it's a PNGSequenceDecoder
    Result result = decoder->open("frames/image_001.png");

    if (result == Result::Success) {
        std::cout << "Using decoder type: " << MediaTypeToString(decoder->getMediaType())
                  << std::endl;

        DecodedFrame frame;
        decoder->decodeFrame(0, frame);

        if (frame.valid) {
            std::cout << "Frame decoded successfully" << std::endl;
        }
    }
}

// ============================================================================
// Example 5: Integration with ECS (Conceptual)
// ============================================================================

// Conceptual DecodeSystem using PNGSequenceDecoder
class DecodeSystem {
private:
    std::unordered_map<EntityID, std::unique_ptr<Decoder>> m_decoders;
    entt::registry& m_registry;

public:
    DecodeSystem(entt::registry& registry) : m_registry(registry) {}

    // Initialize decoder for a clip entity
    void initializeClip(EntityID entityID, const std::string& filepath) {
        auto mediaType = detectMediaType(filepath);
        auto decoder = createDecoder(mediaType);

        if (decoder && decoder->open(filepath) == Result::Success) {
            m_decoders[entityID] = std::move(decoder);
        }
    }

    // Decode frames for all clips in view
    void update(FrameNumber targetFrame) {
        // Iterate through entities with Clip and FrameBuffer components
        auto view = m_registry.view<Clip, FrameBuffer>();

        for (auto [entity, clip, buffer] : view.each()) {
            EntityID entityID = static_cast<EntityID>(entity);

            // Get decoder for this clip
            auto it = m_decoders.find(entityID);
            if (it == m_decoders.end()) {
                continue;  // No decoder initialized
            }

            Decoder* decoder = it->second.get();

            // Check if we need to decode this frame
            if (buffer.targetFrame == targetFrame) {
                continue;  // Already have this frame
            }

            // Seek and decode
            if (decoder->seek(targetFrame) == Result::Success) {
                DecodedFrame decodedFrame;

                if (decoder->decodeFrame(targetFrame, decodedFrame) == Result::Success) {
                    // Store decoded frame in ring buffer
                    buffer.push(std::move(decodedFrame));
                }
            }
        }
    }

    // Cleanup decoders
    void shutdown() {
        m_decoders.clear();
    }
};

// ============================================================================
// Example 6: Working with Alpha Channels
// ============================================================================

void example_alpha_handling() {
    PNGSequenceDecoder decoder;

    if (decoder.open("sequences/transparent_001.png") != Result::Success) {
        std::cerr << "Failed to open sequence" << std::endl;
        return;
    }

    std::cout << "Sequence has alpha channel: " << (decoder.hasAlpha() ? "yes" : "no")
              << std::endl;

    DecodedFrame frame;
    if (decoder.decodeFrame(0, frame) != Result::Success) {
        std::cerr << "Failed to decode frame" << std::endl;
        return;
    }

    // Analyze alpha channel
    std::cout << "\nAlpha Channel Analysis:" << std::endl;

    uint8_t* pixelData = frame.data.data();
    uint32_t pixelCount = frame.width * frame.height;

    uint32_t fullyOpaque = 0;      // Alpha == 255
    uint32_t fullyTransparent = 0; // Alpha == 0
    uint32_t semiTransparent = 0;  // 0 < Alpha < 255

    for (uint32_t i = 0; i < pixelCount; ++i) {
        uint8_t alpha = pixelData[i * 4 + 3];

        if (alpha == 255) {
            fullyOpaque++;
        }
        else if (alpha == 0) {
            fullyTransparent++;
        }
        else {
            semiTransparent++;
        }
    }

    std::cout << "  Fully Opaque: " << fullyOpaque << " pixels" << std::endl;
    std::cout << "  Fully Transparent: " << fullyTransparent << " pixels" << std::endl;
    std::cout << "  Semi-transparent: " << semiTransparent << " pixels" << std::endl;

    // Note: Alpha is premultiplied, so RGB values have already been multiplied by A/255
    std::cout << "\nNote: RGB values are PREMULTIPLIED by alpha for GPU blending." << std::endl;
    std::cout << "This means RGB has been multiplied by (A/255) already." << std::endl;
}

// ============================================================================
// Example 7: Performance Measurement
// ============================================================================

#include <chrono>

void example_performance_measurement() {
    PNGSequenceDecoder decoder;

    if (decoder.open("sequences/performance_test_001.png") != Result::Success) {
        std::cerr << "Failed to open sequence" << std::endl;
        return;
    }

    auto startTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;

    // Decode first 100 frames and measure time
    for (FrameNumber i = 0; i < std::min(100LL, decoder.getDuration()); ++i) {
        DecodedFrame frame;

        if (decoder.decodeFrame(i, frame) == Result::Success) {
            frameCount++;
        }
    }

    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

    double framesPerSecond = (frameCount * 1000.0) / duration.count();

    std::cout << "Decoded " << frameCount << " frames in " << duration.count() << " ms"
              << std::endl;
    std::cout << "Performance: " << framesPerSecond << " frames/second" << std::endl;

    if (framesPerSecond >= 30.0) {
        std::cout << "Result: GOOD (can sustain 30 fps playback)" << std::endl;
    }
    else {
        std::cout << "Result: POOR (cannot sustain 30 fps playback)" << std::endl;
        std::cout << "Recommendation: Consider caching or hardware decoding" << std::endl;
    }
}

// ============================================================================
// Example 8: Handling Missing or Corrupted Files
// ============================================================================

void example_robustness() {
    std::vector<std::string> testPaths = {
        "sequences/good/frame_001.png",      // Valid sequence
        "sequences/corrupt/frame_001.png",   // File exists but is corrupted PNG
        "sequences/missing/frame_001.png",   // Directory doesn't exist
        "",                                  // Empty path
    };

    for (const auto& path : testPaths) {
        std::cout << "\nTesting: " << (path.empty() ? "(empty)" : path) << std::endl;

        PNGSequenceDecoder decoder;
        Result result = decoder.open(path);

        if (result == Result::Success) {
            std::cout << "  Status: OK (" << decoder.getDuration() << " frames)" << std::endl;

            // Try to decode first frame
            DecodedFrame frame;
            Result decodeResult = decoder.decodeFrame(0, frame);

            if (decodeResult == Result::Success) {
                std::cout << "  Frame 0: OK" << std::endl;
            }
            else {
                std::cout << "  Frame 0: FAILED - " << ResultToString(decodeResult) << std::endl;
            }
        }
        else {
            std::cout << "  Status: FAILED - " << ResultToString(result) << std::endl;
        }
    }
}

} // namespace entity::examples
