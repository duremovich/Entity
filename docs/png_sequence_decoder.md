# PNGSequenceDecoder Implementation

## Overview

The `PNGSequenceDecoder` class implements support for loading and decoding PNG image sequences. It is designed for Phase 4 of the Entity Media Server development roadmap and provides a lightweight alternative to video codecs for situations where PNG sequences are more appropriate.

## Features

- **Directory Scanning**: Automatically discovers all PNG files in a directory
- **Alphabetical Ordering**: Sorts files alphabetically to maintain sequence order
- **Alpha Channel Support**: Detects and preserves PNG alpha channels
- **Premultiplied Alpha**: Automatically converts straight alpha to premultiplied alpha for GPU rendering
- **RGBA Format**: All frames are converted to RGBA8 format for consistency
- **On-Demand Decoding**: Frames are decoded when requested (not pre-buffered)
- **Thread-Safe Architecture**: Each decoder instance is designed for single-threaded use (no locks needed)

## Architecture

### Class Structure

```cpp
class PNGSequenceDecoder : public Decoder {
    // Implements all pure virtual methods from Decoder base class
    Result open(const std::string& filepath) override;
    Result decodeFrame(FrameNumber frameNumber, DecodedFrame& outFrame) override;
    Result seek(FrameNumber frameNumber) override;
    // ... getters ...
};
```

### Data Members

| Member | Type | Purpose |
|--------|------|---------|
| `m_basePath` | `std::string` | Directory containing PNG sequence |
| `m_fileList` | `std::vector<std::string>` | Sorted list of PNG file paths |
| `m_isOpen` | `bool` | Decoder state flag |
| `m_width`, `m_height` | `uint32_t` | Frame dimensions (from first frame) |
| `m_frameRate` | `double` | Frame rate (default 30 fps) |
| `m_duration` | `FrameNumber` | Number of frames in sequence |
| `m_hasAlpha` | `bool` | Whether frames have meaningful alpha channel |
| `m_currentFrame` | `FrameNumber` | Last seeked frame number |

## Usage

### Basic Usage

```cpp
#include "entity/media/PNGSequenceDecoder.hpp"

entity::PNGSequenceDecoder decoder;

// Open a PNG sequence
entity::Result result = decoder.open("C:\\path\\to\\frames\\frame.png");
if (result != entity::Result::Success) {
    std::cerr << "Failed to open sequence" << std::endl;
    return;
}

// Decode a specific frame
entity::DecodedFrame frame;
result = decoder.decodeFrame(0, frame);
if (result == entity::Result::Success) {
    // frame.data contains RGBA pixels
    // frame.width and frame.height are set
    uint32_t* pixels = reinterpret_cast<uint32_t*>(frame.data.data());
}

// Clean up
decoder.close();
```

### Integration with Factory

The `createDecoder()` factory function automatically creates a `PNGSequenceDecoder` when `MediaType::PNGSequence` is requested:

```cpp
std::unique_ptr<Decoder> decoder = createDecoder(MediaType::PNGSequence);
decoder->open("sequence/frame_001.png");
```

### Type Detection

The `detectMediaType()` function automatically recognizes PNG files:

```cpp
MediaType type = detectMediaType("frame_001.png");
// Returns MediaType::PNGSequence
```

## Implementation Details

### Directory Scanning

The `scanDirectory()` method:

1. Takes the provided filepath and extracts its directory
2. Handles both file paths and directory paths
3. Iterates through directory contents looking for `.png` files (case-insensitive)
4. Sorts results alphabetically (assumes numbered naming convention)
5. Returns `Result::FileNotFound` if no PNGs are found

**Naming Convention**: The implementation expects PNG sequences to follow standard naming conventions:
- `frame_001.png`, `frame_002.png`, ... (leading zeros recommended)
- `image_001.png`, `image_002.png`, ...
- `seq_0001.png`, `seq_0002.png`, ...

The alphabetical sort ensures correct sequence order for zero-padded filenames.

### PNG Decoding

The `loadPNG()` method:

1. **File Reading**: Loads entire PNG file into memory
2. **Decoding**: Uses stb_image to decode PNG to RGBA
   - `stbi_load_from_memory()` with forced 4-channel output
   - Handles various PNG formats (8-bit, 16-bit, palette, etc.)
3. **Alpha Detection**: Checks if image has meaningful alpha
4. **Premultiplication**: Converts straight alpha to premultiplied
5. **Memory Management**: Frees stb_image buffers via `stbi_image_free()`

### Premultiplied Alpha

The `premultiplyAlpha()` method converts each pixel from straight alpha to premultiplied:

```cpp
// Input: RGBA with straight alpha
// Output: RGBA with premultiplied alpha

R_out = (R_in * A_in) / 255
G_out = (G_in * A_in) / 255
B_out = (B_in * A_in) / 255
A_out = A_in
```

This conversion is necessary for correct GPU blending in the render pipeline.

## Performance Characteristics

### Memory Usage

- **Directory Scan**: O(n) where n = number of files in directory
- **Frame Decoding**: O(w * h) where w,h = frame dimensions
- **Per-Frame Memory**: width * height * 4 bytes (RGBA8)
- **Example**: 1920x1080 RGBA = 8.3 MB per frame

### CPU Usage

- **Directory Scan**: Minimal (one-time operation)
- **PNG Decoding**: High (CPU-intensive, depends on PNG compression)
- **Typical**: 1920x1080 PNG decode = 5-15 ms on modern CPU
- **Premultiplication**: 0.5-2 ms (simple multiplication per pixel)

### Optimization Notes

1. **Caching**: Consider caching decoded frames if frequently accessed
2. **Threading**: Each decoder instance can be used by a single decode thread
3. **Sequential Access**: If frames are always accessed sequentially, pre-decode next frame
4. **Compression**: Use PNG compression level 6-8 for smaller files

## Dependencies

### Required Libraries

- **stb_image**: Single-header PNG decoder
  - Included via vcpkg dependency "stb"
  - Header: `stb_image.h`
  - License: Public domain / MIT

### Standard Library

- `<filesystem>` - Directory iteration
- `<fstream>` - File I/O
- `<algorithm>` - std::sort, std::transform
- `<cstring>` - memcpy
- `<iostream>` - Logging

## Error Handling

All errors return `Result` enum codes:

| Code | Meaning | Recovery |
|------|---------|----------|
| `Result::Success` | Operation succeeded | Continue normally |
| `Result::FileNotFound` | PNG file or directory not found | Check file path |
| `Result::DecoderError` | PNG decode failed (corrupt file) | Try another file |
| `Result::InvalidParameter` | Frame number out of range | Check frame bounds |
| `Result::Failure` | Generic failure (decoder not open) | Call open() first |

### Example Error Handling

```cpp
Result result = decoder.open(filepath);
if (result == Result::FileNotFound) {
    std::cerr << "PNG sequence not found: " << filepath << std::endl;
} else if (result == Result::DecoderError) {
    std::cerr << "Failed to decode PNG file" << std::endl;
} else if (result == Result::Success) {
    // Continue
}
```

## Threading Model

### Design

- **NOT thread-safe by design**
- Each decoder instance should be owned by a single decode thread
- The `DecodeSystem` manages decoder lifecycle and threading
- No locks or atomics needed (single-threaded per instance)

### Typical Usage

```cpp
// In DecodeSystem (runs on decode thread)
class DecodeSystem {
    std::unordered_map<EntityID, std::unique_ptr<Decoder>> m_decoders;

    void decodeClip(EntityID entityID) {
        auto decoder = m_decoders[entityID].get();  // Single decoder per clip
        decoder->decodeFrame(frameNum, frame);      // No contention
    }
};
```

## Integration with ECS

### Components

The decoder is used by these components:

- **`Clip`** - Holds reference to media file
- **`FrameBuffer`** - Stores decoded frames
- **`VideoTexture`** - GPU resource created from decoded frames

### Systems

The decoder is used by:

- **`DecodeSystem`** - Manages decoder instances, calls decodeFrame()
- **`CompositorSystem`** - Reads decoded frames from FrameBuffer

### Data Flow

```
Clip (filepath) → DecodeSystem → Decoder.decodeFrame()
                                      ↓
                                 DecodedFrame
                                      ↓
                                FrameBuffer (ring buffer)
                                      ↓
                                CompositorSystem
                                      ↓
                                VideoTexture → GPU
```

## Testing

The implementation includes comprehensive unit tests in `tests/test_png_sequence_decoder.cpp`:

- Initialization and state
- Directory scanning
- Error handling (missing files, invalid parameters)
- File ordering consistency
- Properties (frame rate, media type, alpha channel)

### Running Tests

```bash
# Build with tests enabled
cmake -B build -DBUILD_TESTS=ON
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure
```

## Future Enhancements

1. **Pattern-Based Naming**: Support printf-style patterns (frame_%03d.png)
2. **DPX Sequence Support**: Similar implementation for DPX sequences
3. **Caching Layer**: Cache decoded frames in memory
4. **Tiled Decoding**: For very large images, decode in tiles
5. **Hardware Acceleration**: GPU-based PNG decompression (future)

## Common Issues and Solutions

### Issue: "No PNG files found in directory"

**Cause**: PNG files not in the specified directory
**Solution**:
- Verify file path is correct
- Check file extensions are lowercase `.png`
- Ensure directory permissions allow reading

### Issue: "Failed to decode PNG: Invalid or truncated PNG"

**Cause**: PNG file is corrupted or stb_image doesn't support the format
**Solution**:
- Verify PNG file integrity with external tool
- Try re-exporting from original source
- Check for unsupported PNG variants

### Issue: Frames appear washed out or semi-transparent

**Cause**: Premultiplied alpha expected but straight alpha provided (or vice versa)
**Solution**:
- This decoder always outputs premultiplied alpha
- Ensure rendering pipeline expects premultiplied alpha
- Check PNG export settings in image editor

## References

- **stb_image documentation**: https://github.com/nothings/stb/blob/master/stb_image.h
- **PNG specification**: http://www.libpng.org/pub/png/spec/
- **Premultiplied alpha**: https://en.wikipedia.org/wiki/Alpha_compositing

## Files

| File | Purpose |
|------|---------|
| `include/entity/media/PNGSequenceDecoder.hpp` | Public header |
| `src/media/PNGSequenceDecoder.cpp` | Implementation |
| `tests/test_png_sequence_decoder.cpp` | Unit tests |
| `CMakeLists.txt` | Build configuration |
| `vcpkg.json` | Dependency manifest (includes stb) |
