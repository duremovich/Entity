# Build Instructions - Entity Media Server

## Prerequisites

### Required Software

1. **Visual Studio 2022** (or Visual Studio Build Tools 2022)
   - Install from: https://visualstudio.microsoft.com/
   - Required components:
     - Desktop development with C++
     - Windows 10/11 SDK (latest)
     - CMake tools for Windows

2. **CMake** 3.21 or newer
   - Install via Visual Studio installer, OR
   - Download from: https://cmake.org/download/
   - OR install via winget: `winget install Kitware.CMake`

3. **vcpkg** (C++ package manager)
   - See installation steps below

### Installing vcpkg

```bash
# Clone vcpkg to C:\vcpkg (recommended location)
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg

# Navigate to vcpkg directory
cd C:\vcpkg

# Bootstrap vcpkg
.\bootstrap-vcpkg.bat

# Integrate with Visual Studio (optional but recommended)
.\vcpkg integrate install
```

This will display a path like:
```
CMake projects should use: -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

Save this path for the configuration step.

## Building the Project

### Step 1: Configure with CMake

Open a command prompt or PowerShell in the project directory:

```bash
cd "Entity"
```

Configure the project (replace the vcpkg path if you installed it elsewhere):

```bash
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
```

**Note**: The first configuration will take **10-30 minutes** as vcpkg downloads and compiles all dependencies:
- FFmpeg (large, complex build)
- EnTT (header-only, fast)
- GLFW3 (medium)
- ImGui (fast)
- GLM (header-only, fast)

You'll see output like:
```
Detecting vcpkg-manifest mode...
Installing dependencies...
Building ffmpeg[core,avcodec,avformat,swscale,swresample]:x64-windows...
```

### Step 2: Build the Project

After configuration completes successfully:

```bash
cmake --build build --config Release
```

For debug builds:
```bash
cmake --build build --config Debug
```

Build times:
- First build: ~2-5 minutes (compiling project code)
- Incremental builds: ~10-30 seconds (only changed files)

### Step 3: Run the Application

After successful build:

**Release:**
```bash
.\build\bin\Release\EntityMediaEditor.exe
```

**Debug:**
```bash
.\build\bin\Debug\EntityMediaEditor.exe
```

You should see a window open with the title "Entity Media Server - Editor".

## Build Options

You can customize the build with CMake options:

```bash
# Disable tests
cmake -B build -S . -DBUILD_TESTS=OFF -DCMAKE_TOOLCHAIN_FILE=...

# Disable D3D12 debug layer (for release builds)
cmake -B build -S . -DENABLE_D3D12_DEBUG=OFF -DCMAKE_TOOLCHAIN_FILE=...
```

## Troubleshooting

**For comprehensive troubleshooting information, see [docs/troubleshooting.md](docs/troubleshooting.md)**

Common issues are listed below for quick reference:

### vcpkg fails to find packages

**Problem**: CMake can't find dependencies even though vcpkg is installed.

**Solution**:
1. Ensure you specified `-DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake`
2. Verify vcpkg.json is in the project root
3. Try deleting the build directory and reconfiguring:
   ```bash
   rmdir /s /q build
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
   ```

### FFmpeg not found

**Problem**: CMake can't find FFmpeg libraries.

**Solution**:
1. Check vcpkg installed FFmpeg:
   ```bash
   C:\vcpkg\vcpkg list | findstr ffmpeg
   ```
2. If not listed, manually install:
   ```bash
   C:\vcpkg\vcpkg install ffmpeg[avcodec,avformat,swscale,swresample]:x64-windows
   ```

### DXC (DirectX Shader Compiler) not found

**Problem**: Shader compilation fails with "DXC not found".

**Solution**:
1. Ensure Windows SDK 10.0.20348.0 or later is installed
2. Check if DXC exists:
   ```bash
   where dxc
   ```
3. If not found, install Windows SDK from Visual Studio Installer
4. Alternatively, download DirectX Shader Compiler from:
   https://github.com/microsoft/DirectXShaderCompiler/releases

### Build fails with "cannot find d3d12.lib"

**Problem**: Linker can't find D3D12 libraries.

**Solution**:
1. Ensure Windows 10/11 SDK is installed
2. Verify Visual Studio has "Desktop development with C++" workload
3. Try running from "x64 Native Tools Command Prompt for VS 2022"

### ImGui-related errors

**Problem**: Errors about ImGui D3D12 backend.

**Solution**:
1. Check vcpkg.json includes imgui features:
   ```json
   {
     "name": "imgui",
     "features": ["docking-experimental", "dx12-binding", "glfw-binding"]
   }
   ```
2. Clean and reconfigure if features were added after first config

## Development Workflow

### Using Visual Studio

1. Open the generated solution:
   ```bash
   start build\EntityMediaServer.sln
   ```

2. Set EntityMediaEditor as startup project:
   - Right-click on EntityMediaEditor → "Set as Startup Project"

3. Build with `Ctrl+Shift+B`

4. Run with `F5` (debug) or `Ctrl+F5` (without debugger)

### Using VS Code

1. Install recommended extensions:
   - C/C++ (Microsoft)
   - CMake Tools (Microsoft)

2. Open project folder in VS Code

3. Configure CMake:
   - `Ctrl+Shift+P` → "CMake: Configure"
   - Select Visual Studio toolchain

4. Build:
   - `Ctrl+Shift+P` → "CMake: Build"
   - Or click "Build" in the status bar

5. Run:
   - `Ctrl+Shift+P` → "CMake: Debug"
   - Or use the debugger panel (`F5`)

### Command Line (Ninja - Faster)

For faster incremental builds, use Ninja generator:

```bash
# Install Ninja
winget install Ninja-build.Ninja

# Configure with Ninja
cmake -B build -S . -G Ninja -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build

# Subsequent builds are much faster
cmake --build build
```

## Clean Build

To start fresh:

```bash
# Delete build directory
rmdir /s /q build

# Delete vcpkg installed packages (optional, forces re-download)
rmdir /s /q vcpkg_installed

# Reconfigure and rebuild
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## Performance Tips

1. **Use Release builds for testing playback performance**
   ```bash
   cmake --build build --config Release
   ```

2. **Enable parallel compilation**
   - Visual Studio: Automatically enabled
   - Command line: `cmake --build build --config Release -- /m`

3. **Use SSD for build directory**
   - Significantly faster than HDD

4. **Close other applications during first vcpkg install**
   - FFmpeg compilation is CPU and disk intensive

---

## Next Steps After Successful Build

Once the application builds and runs:

1. ✅ Window opens successfully
2. ✅ No crash on startup
3. ✅ Clean exit when window is closed

You're ready to continue to Phase 2: Implementing the D3D12 renderer!

See [CLAUDE.md](CLAUDE.md) for the full development roadmap.
