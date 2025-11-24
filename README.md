# Entity Media Server

A high-performance, cross-platform realtime media playback and projection-mapping engine built with modern C++, designed for professional live events and installations.

## Overview

Entity Media Server is a professional-grade media server similar to Disguise (d3) and Watchout, with compositing concepts borrowed from After Effects. Built from the ground up for performance using an Entity Component System (ECS) architecture and Data-Oriented Design (DoD), it delivers frame-accurate playback of multiple video layers with GPU-accelerated compositing.

### Key Features

- **Multi-Layer Playback**: Real-time playback of multiple video and image layers with transparency
- **Professional Codecs**: Native support for ProRes 4444 (with alpha), HAP, and PNG sequences
- **GPU Compositing**: Hardware-accelerated compositing with blend modes and effects
- **Frame-Accurate Timeline**: Precision timeline with play/pause/scrub controls
- **Multi-Output Support**: Persistent display mapping with EDID-based identification
- **3D Visualizer**: Virtual screen placement for projection mapping (future)
- **Network Sync**: Multi-node synchronized playback (future)

## Technology Stack

- **Language**: C++20
- **Build System**: CMake 3.21+
- **Package Manager**: vcpkg
- **ECS Framework**: EnTT
- **Graphics API**: Direct3D 12 (Windows), Metal (macOS - future)
- **Windowing**: GLFW
- **Media Decoding**: FFmpeg
- **UI**: Dear ImGui

## System Requirements

### Minimum
- **OS**: Windows 10/11 (64-bit)
- **CPU**: Intel Core i5 or AMD Ryzen 5
- **GPU**: DirectX 12 compatible (NVIDIA GTX 1060 / AMD RX 580 or better)
- **RAM**: 16 GB
- **Storage**: SSD recommended for media playback

### Recommended
- **OS**: Windows 11 (64-bit)
- **CPU**: Intel Core i7/i9 or AMD Ryzen 7/9
- **GPU**: NVIDIA RTX 3060 or better
- **RAM**: 32 GB or more
- **Storage**: NVMe SSD

## Build Instructions

### Prerequisites

1. **Install Visual Studio 2022** (or Visual Studio Build Tools)
   - Include "Desktop development with C++"
   - Include Windows 10/11 SDK

2. **Install CMake** (3.21 or newer)
   ```bash
   winget install Kitware.CMake
   ```

3. **Install vcpkg**
   ```bash
   git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
   cd C:\vcpkg
   .\bootstrap-vcpkg.bat
   .\vcpkg integrate install
   ```

### Build Steps

1. **Clone the repository** (or navigate to project directory)
   ```bash
   cd "Entity"
   ```

2. **Configure with CMake**
   ```bash
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
   ```

3. **Build the project**
   ```bash
   cmake --build build --config Release
   ```

4. **Run the application**
   ```bash
   .\build\bin\Release\EntityMediaEditor.exe
   ```

### Debug Build

For development and debugging:
```bash
cmake --build build --config Debug
.\build\bin\Debug\EntityMediaEditor.exe
```

## Project Structure

```
EntityMediaServer/
├── CLAUDE.md              # Detailed development roadmap
├── README.md              # This file
├── CMakeLists.txt         # Root CMake configuration
├── vcpkg.json             # Dependency manifest
│
├── include/entity/        # Public API headers
│   ├── core/              # Core engine
│   ├── components/        # ECS components
│   ├── media/             # Media decoding
│   ├── render/            # Rendering
│   └── timeline/          # Timeline engine
│
├── src/                   # Implementation files
│   ├── core/
│   ├── media/
│   ├── render/
│   ├── timeline/
│   └── systems/
│
├── apps/                  # Applications
│   └── editor/            # Main editor application
│
├── shaders/               # HLSL shaders
├── tests/                 # Unit tests
└── docs/                  # Documentation
```

## Development

### Architecture

Entity Media Server uses an **Entity Component System (ECS)** architecture powered by EnTT, combined with Data-Oriented Design principles for maximum performance.

**Key Components**:
- `Transform` - Spatial positioning and scaling
- `MediaLayer` - Layer properties (opacity, blend mode, z-order)
- `Clip` - Media source reference
- `VideoTexture` - GPU texture resources
- `FrameBuffer` - Decoded frame buffer

**Key Systems**:
- `DecodeSystem` - FFmpeg-based media decoding
- `CompositorSystem` - Multi-layer GPU compositing
- `RenderSystem` - D3D12 rendering pipeline
- `TimelineSystem` - Timeline and transport control

### Code Style

- Modern C++20 features
- PascalCase for classes (`D3D12Renderer`)
- camelCase for functions and variables (`uploadTexture()`)
- Data-oriented hot paths
- Minimal allocations in update loops

### Development Roadmap

See [CLAUDE.md](CLAUDE.md) for the detailed development roadmap and current progress.

## Current Status

🚧 **In Development - MVP Phase**

### Completed
- ✅ Project scaffold and build system
- ✅ Dependency management with vcpkg
- ✅ Folder structure and organization

### In Progress
- 🔄 Core engine initialization
- 🔄 D3D12 renderer setup
- 🔄 ECS component definitions

### Next Steps
- ⬜ FFmpeg decoder integration
- ⬜ Timeline engine
- ⬜ Multi-layer compositor
- ⬜ ImGui debug interface

## Supported Media Formats

### Video Codecs (Current/Planned)
- ✅ **ProRes 4444** - Apple ProRes with alpha channel
- ✅ **HAP** - Vidvox HAP codec (GPU-accelerated)
- ⬜ **HAP Alpha** - HAP with alpha channel
- ⬜ **HAP Q** - High-quality HAP variant

### Image Formats
- ✅ **PNG Sequences** - Image sequences with alpha
- ⬜ **DPX** - Digital Picture Exchange (future)
- ⬜ **EXR** - OpenEXR sequences (future)

### Hardware Acceleration (Future)
- ⬜ NVDEC (NVIDIA)
- ⬜ Quick Sync (Intel)
- ⬜ DXVA (Generic Windows)

## Performance

### Targets
- **Playback**: 60 FPS (vsync) for HD content
- **Latency**: < 3 frames from decode to display
- **Layers**: 8+ simultaneous layers (hardware dependent)

### Optimizations
- Lock-free frame buffers
- Zero-copy GPU upload (where supported)
- Cache-friendly component iteration
- Async decode threads per clip
- GPU-accelerated compositing

## License

Proprietary - Internal Use Only

This is proprietary software developed for internal use. All rights reserved.

## Contributing

This is an internal project. For development questions or issues, consult the [CLAUDE.md](CLAUDE.md) roadmap or contact the development team.

## Resources

- **Documentation**: See `/docs` folder
- **Architecture**: See [CLAUDE.md](CLAUDE.md)
- **Build System**: [CMake Documentation](https://cmake.org/documentation/)
- **ECS Framework**: [EnTT Documentation](https://github.com/skypjack/entt)
- **D3D12**: [Microsoft D3D12 Docs](https://learn.microsoft.com/en-us/windows/win32/direct3d12/)

---

**Status**: Pre-Alpha Development
**Version**: 0.1.0
**Last Updated**: November 2024
