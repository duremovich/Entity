# Phase 1: Project Scaffold & Dependencies

**Status**: IN PROGRESS
**Started**: 2024 (Initial)
**Expected Completion**: Awaiting build validation

---

## Overview

Phase 1 establishes the foundation of the Entity Media Server project by setting up the build system, dependency management, and folder structure. This phase creates the scaffolding necessary for all future development.

---

## Goals

- Set up CMake project structure
- Configure vcpkg for dependency management
- Create initial folder hierarchy
- Get basic build system working
- Ensure all dependencies are properly integrated

---

## Tasks

### 1. Create Root CMakeLists.txt

**Status**: COMPLETE

**Details**:
- Set C++20 as the language standard
- Configure vcpkg toolchain integration
- Set up subdirectory includes for src/, apps/, shaders/, tests/
- Configure project-wide compiler settings
- Set output directories for binaries and libraries

**Files Created**:
- `CMakeLists.txt` (root)

**Key Configurations**:
```cmake
cmake_minimum_required(VERSION 3.21)
project(EntityMediaServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# vcpkg integration happens via CMAKE_TOOLCHAIN_FILE
```

---

### 2. Create vcpkg.json Manifest

**Status**: COMPLETE

**Details**:
- Define all project dependencies in manifest mode
- Specify features for FFmpeg (avcodec, avformat, swscale, swresample)
- Include EnTT, GLFW3, ImGui, GLM, DirectX headers

**Files Created**:
- `vcpkg.json`

**Dependencies**:
- **EnTT**: Entity Component System framework
- **FFmpeg**: Video decoding (with avcodec, avformat, swscale, swresample)
- **GLFW3**: Windowing system
- **ImGui**: User interface
- **GLM**: Mathematics library
- **DirectX headers**: D3D12 development

---

### 3. Create Folder Structure

**Status**: COMPLETE

**Details**:
- Create all directories specified in project structure
- Add placeholder README files where appropriate
- Organize by functional area (core, media, render, timeline, systems)

**Directories Created**:
```
EntityMediaServer/
├── cmake/                  # CMake helper modules
├── include/entity/         # Public API headers
│   ├── core/
│   ├── components/
│   ├── media/
│   ├── render/
│   └── timeline/
├── src/                    # Implementation files
│   ├── core/
│   ├── media/
│   ├── render/
│   ├── timeline/
│   └── systems/
├── apps/                   # Executables
│   ├── editor/
│   └── player/
├── shaders/                # HLSL shaders
├── tests/                  # Unit tests
└── docs/                   # Documentation
    └── phases/
```

---

### 4. Create .gitignore

**Status**: COMPLETE

**Details**:
- Ignore C++ build artifacts (*.o, *.obj, *.exe, *.dll, *.lib)
- Ignore CMake build directory (build/, out/)
- Ignore vcpkg installed packages (vcpkg_installed/)
- Ignore IDE files (Visual Studio, VS Code)
- Ignore OS files (.DS_Store, Thumbs.db)

**Files Created**:
- `.gitignore`

---

### 5. Create Basic README.md

**Status**: COMPLETE

**Details**:
- Project description and overview
- Feature list
- Basic build instructions
- Quick start guide
- Links to additional documentation

**Files Created**:
- `README.md`
- `BUILD.md` (detailed build instructions)

---

### 6. Create CMake Helper Modules

**Status**: COMPLETE

**Details**:
- **FindFFmpeg.cmake**: Locate FFmpeg libraries and headers
- **CompileShaders.cmake**: Automate HLSL shader compilation with DXC
- **CompilerWarnings.cmake**: Configure warning levels and strictness (optional)

**Files Created**:
- `cmake/FindFFmpeg.cmake`
- `cmake/CompileShaders.cmake`

**FindFFmpeg.cmake Features**:
- Find libavcodec, libavformat, libswscale, libswresample
- Set include directories and library paths
- Create imported targets for easy linking

**CompileShaders.cmake Features**:
- Locate DXC (DirectX Shader Compiler)
- Compile HLSL to .cso (compiled shader object) files
- Support vertex shaders (vs_6_0) and pixel shaders (ps_6_0)
- Integrate with build system

---

## Build Commands

### First-Time Setup

```bash
# Install vcpkg (if not already installed)
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\vcpkg\vcpkg integrate install
```

### Configure Project

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

**Alternative with preset** (if CMakePresets.json exists):
```bash
cmake --preset windows-debug
```

### Build

```bash
# Debug build
cmake --build build --config Debug

# Release build
cmake --build build --config Release
```

### Run

```bash
# Debug
.\build\bin\Debug\EntityMediaEditor.exe

# Release
.\build\bin\Release\EntityMediaEditor.exe
```

---

## Validation Checklist

This phase is complete when all of the following are verified:

- [ ] **CMake configures without errors**
  - Run `cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=...`
  - No errors reported
  - All dependencies found

- [ ] **All dependencies found by vcpkg**
  - EnTT located
  - FFmpeg located (with avcodec, avformat, swscale, swresample)
  - GLFW3 located
  - ImGui located
  - GLM located
  - DirectX headers located

- [ ] **Project builds successfully**
  - Run `cmake --build build --config Release`
  - No compilation errors
  - EntityMediaEditor.exe created

- [ ] **Executable runs**
  - Run `.\build\bin\Release\EntityMediaEditor.exe`
  - Application starts without crashing
  - Even if window is empty or minimal, no errors on launch

---

## Common Issues

### vcpkg fails to find packages

**Symptoms**: CMake cannot find EnTT, FFmpeg, etc.

**Solutions**:
- Ensure vcpkg toolchain file is correctly specified in CMake command
- Run `C:\vcpkg\vcpkg install` manually to verify vcpkg is working
- Check `vcpkg.json` syntax for errors
- Try `C:\vcpkg\vcpkg integrate install` again

### CMake configuration fails

**Symptoms**: CMake configure step fails with errors

**Solutions**:
- Check CMake version (must be 3.21+)
- Verify Visual Studio 2022 or VS Build Tools installed
- Ensure Windows SDK installed (for D3D12)
- Delete `build/` directory and try again

### FFmpeg linking errors

**Symptoms**: Build succeeds but linker fails to find FFmpeg symbols

**Solutions**:
- Verify FFmpeg features enabled in vcpkg.json
- Check `cmake/FindFFmpeg.cmake` module
- Ensure all FFmpeg libraries specified (avcodec, avformat, swscale, swresample)
- Try manually specifying FFmpeg paths in CMakeLists.txt

### Build succeeds but executable won't run

**Symptoms**: EntityMediaEditor.exe crashes immediately

**Solutions**:
- Check that DLLs are in same directory as .exe or in PATH
- Run from Visual Studio debugger to see error messages
- Verify all vcpkg dependencies installed correctly
- Check Windows Event Viewer for crash details

---

## Next Steps

After Phase 1 is complete and validated:

1. **Proceed to Phase 2**: Core Engine & Window
   - See `docs/phases/phase-02-core-engine.md`
   - Set up GLFW window
   - Initialize D3D12 device and swap chain
   - Create EnTT registry
   - Implement main render loop

2. **Set up development environment**:
   - Configure VS Code or Visual Studio
   - Install debugging tools (RenderDoc, PIX)
   - Set up git hooks (if desired)

3. **Review architecture**:
   - Read `docs/architecture.md` (when created)
   - Understand ECS pattern with EnTT
   - Review D3D12 basics

---

## Notes

- This phase is intentionally minimal - just scaffolding
- No actual functionality is implemented yet
- Focus is on getting build system working reliably
- All validation must pass before moving to Phase 2
- If build issues persist, see `docs/troubleshooting.md`

---

**Phase Owner**: Foundation Team
**Dependencies**: None (first phase)
**Blocks**: All other phases
**Last Updated**: 2024-11-24
