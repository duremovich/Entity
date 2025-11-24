# Entity Media Server - Troubleshooting Guide

This document contains solutions to common issues encountered during development and runtime.

---

## Table of Contents

- [Build System Issues](#build-system-issues)
- [Dependency Issues](#dependency-issues)
- [D3D12 Issues](#d3d12-issues)
- [FFmpeg Issues](#ffmpeg-issues)
- [Runtime Issues](#runtime-issues)
- [Performance Issues](#performance-issues)

---

## Build System Issues

### CMake Configuration Fails

**Symptoms**:
- `cmake -B build -S .` fails with errors
- "Could not find a package configuration file"
- CMake version errors

**Solutions**:

1. **Check CMake Version**:
   ```bash
   cmake --version
   # Must be 3.21 or higher
   ```
   If too old, download latest from [cmake.org](https://cmake.org/download/)

2. **Verify vcpkg Toolchain File**:
   ```bash
   # Ensure toolchain file path is correct
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
   ```

3. **Delete Build Directory and Retry**:
   ```bash
   rmdir /s /q build
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
   ```

4. **Check Windows SDK Installation**:
   - Open Visual Studio Installer
   - Verify "Windows 10 SDK" or "Windows 11 SDK" is installed
   - Install if missing

---

### Build Succeeds but Linker Fails

**Symptoms**:
- Compilation succeeds
- Linker errors: "unresolved external symbol"
- Missing .lib or .dll files

**Solutions**:

1. **Check Library Linking in CMakeLists.txt**:
   ```cmake
   target_link_libraries(EntityMediaEngine PUBLIC
       d3d12.lib
       dxgi.lib
       dxguid.lib
       # Add missing libraries here
   )
   ```

2. **Verify vcpkg Integration**:
   ```bash
   C:\vcpkg\vcpkg integrate install
   ```

3. **Check vcpkg Triplet**:
   ```bash
   # For 64-bit Windows
   cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=... -DVCPKG_TARGET_TRIPLET=x64-windows
   ```

---

### Shader Compilation Fails

**Symptoms**:
- "DXC not found"
- HLSL compilation errors
- .cso files not generated

**Solutions**:

1. **Check DXC in PATH**:
   ```bash
   where dxc
   # Should show path to dxc.exe
   ```
   If not found, install Windows SDK with DXC support

2. **Verify HLSL Syntax**:
   - Check shader files for syntax errors
   - Ensure shader model is correct (vs_6_0, ps_6_0)

3. **Enable Verbose Shader Compilation**:
   In `cmake/CompileShaders.cmake`, add verbose flag:
   ```cmake
   add_custom_command(
       ...
       COMMAND dxc ... -Qembed_debug -Zi -Fd ...
   )
   ```

4. **Manual Compilation Test**:
   ```bash
   dxc -T ps_6_0 -E main -Fo test.cso shaders/composite_ps.hlsl
   ```

---

## Dependency Issues

### vcpkg Fails to Find Packages

**Symptoms**:
- CMake reports "Could not find EnTT"
- "Could not find FFmpeg"
- Package not available

**Solutions**:

1. **Update vcpkg**:
   ```bash
   cd C:\vcpkg
   git pull
   .\bootstrap-vcpkg.bat
   ```

2. **Check vcpkg.json Syntax**:
   ```json
   {
     "dependencies": [
       "entt",
       "glfw3",
       "imgui",
       "glm"
     ]
   }
   ```
   Ensure no trailing commas or syntax errors

3. **Manually Install Packages**:
   ```bash
   C:\vcpkg\vcpkg install entt:x64-windows
   C:\vcpkg\vcpkg install glfw3:x64-windows
   C:\vcpkg\vcpkg install imgui[dxbinding]:x64-windows
   ```

4. **Check Package Availability**:
   ```bash
   C:\vcpkg\vcpkg search ffmpeg
   # Verify package exists in vcpkg repository
   ```

---

### FFmpeg Not Found or Wrong Version

**Symptoms**:
- "Could not find FFmpeg"
- FFmpeg version too old
- Missing FFmpeg features (avcodec, avformat, etc.)

**Solutions**:

1. **Install FFmpeg with All Features**:
   ```bash
   C:\vcpkg\vcpkg install ffmpeg[avcodec,avformat,swscale,swresample]:x64-windows
   ```

2. **Check FindFFmpeg.cmake**:
   Verify `cmake/FindFFmpeg.cmake` is correctly searching for components:
   ```cmake
   find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h)
   find_library(AVCODEC_LIBRARY avcodec)
   ```

3. **Manually Specify FFmpeg Paths** (last resort):
   ```cmake
   set(FFMPEG_ROOT "C:/vcpkg/installed/x64-windows")
   ```

---

## D3D12 Issues

### D3D12 Debug Layer Errors

**Symptoms**:
- D3D12 validation errors in output window
- "D3D12 ERROR: ID3D12Device::CreateXXX: ..."
- Crashes with cryptic error codes

**Solutions**:

1. **Enable Graphics Tools**:
   - Settings → Apps → Optional Features
   - Add "Graphics Tools" feature
   - Restart computer

2. **Enable D3D12 Debug Layer**:
   ```cpp
   #ifdef _DEBUG
       ComPtr<ID3D12Debug> debugController;
       if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
           debugController->EnableDebugLayer();
       }
   #endif
   ```

3. **Read Error Messages Carefully**:
   - Check Visual Studio Output window
   - Look for detailed error descriptions
   - Google specific D3D12 error codes

4. **Common Validation Errors**:
   - **Command list not closed**: Call `commandList->Close()` before execute
   - **Wrong resource state**: Check resource barriers
   - **Descriptor heap not set**: Call `SetDescriptorHeaps()` before draw
   - **Mismatched formats**: Ensure RTV/SRV formats match resource

---

### Swap Chain Creation Fails

**Symptoms**:
- "CreateSwapChainForHwnd failed"
- Black screen
- DXGI errors

**Solutions**:

1. **Verify HWND is Valid**:
   ```cpp
   HWND hwnd = glfwGetWin32Window(window);
   if (hwnd == nullptr) {
       // Window not created properly
   }
   ```

2. **Check Swap Chain Description**:
   ```cpp
   swapChainDesc.BufferCount = 2; // Must be >= 2 for FLIP model
   swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
   ```

3. **Ensure Command Queue is Created First**:
   Swap chain requires valid command queue

4. **Check for Exclusive Fullscreen Conflicts**:
   ```cpp
   factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
   ```

---

### GPU Device Lost / Removed

**Symptoms**:
- DXGI_ERROR_DEVICE_REMOVED
- DXGI_ERROR_DEVICE_HUNG
- Application crashes after running for a while

**Solutions**:

1. **Check for Resource Leaks**:
   - Ensure all D3D12 resources are released
   - Check reference counts with debug layer

2. **Verify Driver Stability**:
   - Update GPU drivers to latest version
   - Check Windows Event Viewer for driver crashes

3. **Reduce GPU Load**:
   - Simplify shaders temporarily
   - Reduce texture sizes
   - Lower frame rate

4. **Get Removal Reason**:
   ```cpp
   HRESULT hr = device->GetDeviceRemovedReason();
   // Log and investigate specific error code
   ```

---

## FFmpeg Issues

### FFmpeg Linking Errors

**Symptoms**:
- "unresolved external symbol av_XXX"
- Linker errors related to FFmpeg functions
- Missing avcodec, avformat, etc.

**Solutions**:

1. **Verify FFmpeg Libraries Linked**:
   ```cmake
   target_link_libraries(EntityMediaEngine PRIVATE
       ${FFMPEG_LIBRARIES}
   )
   ```

2. **Check FindFFmpeg.cmake**:
   Ensure all required libraries are found:
   - avcodec
   - avformat
   - avutil
   - swscale
   - swresample

3. **Manually Link FFmpeg**:
   ```cmake
   target_link_libraries(EntityMediaEngine PRIVATE
       avcodec
       avformat
       avutil
       swscale
       swresample
   )
   ```

---

### Codec Not Found or Unsupported

**Symptoms**:
- "Codec not found: prores"
- "Unsupported codec"
- avcodec_find_decoder() returns null

**Solutions**:

1. **Check FFmpeg Build Configuration**:
   ```bash
   ffmpeg -codecs | findstr prores
   # Should show ProRes codec support
   ```

2. **Verify Codec Name**:
   ```cpp
   // Correct codec IDs
   AV_CODEC_ID_PRORES
   AV_CODEC_ID_HAP
   AV_CODEC_ID_PNG
   ```

3. **Install FFmpeg with All Codecs**:
   ```bash
   C:\vcpkg\vcpkg install ffmpeg[avcodec,avformat,swscale]:x64-windows
   ```

---

### Video Decoding Fails or Produces Corrupted Frames

**Symptoms**:
- avcodec_send_packet() fails
- avcodec_receive_frame() returns error
- Frames appear corrupted or garbled

**Solutions**:

1. **Check File Format Support**:
   - Verify input file is valid (play in VLC/FFmpeg)
   - Check codec is supported

2. **Initialize Codec Context Properly**:
   ```cpp
   avcodec_parameters_to_context(codecContext, stream->codecpar);
   if (avcodec_open2(codecContext, codec, nullptr) < 0) {
       // Failed to open codec
   }
   ```

3. **Verify Pixel Format**:
   ```cpp
   // ProRes 4444 uses yuva444p10le
   if (codecContext->pix_fmt == AV_PIX_FMT_YUVA444P10LE) {
       // Correct format for ProRes 4444
   }
   ```

4. **Check for Alpha Channel**:
   ```cpp
   // ProRes 4444 has 4 planes (Y, U, V, A)
   if (frame->data[3] != nullptr) {
       // Alpha channel present
   }
   ```

---

## Runtime Issues

### Application Crashes on Startup

**Symptoms**:
- Immediate crash after launch
- No window appears
- Windows error dialog

**Solutions**:

1. **Run in Debugger**:
   - Open Visual Studio
   - Set breakpoint in main()
   - Step through initialization code
   - Check which init call fails

2. **Check DLL Dependencies**:
   ```bash
   # Use Dependency Walker or similar tool
   dumpbin /dependents EntityMediaEditor.exe
   ```

3. **Verify Working Directory**:
   - Ensure application runs from correct directory
   - Check for missing asset files

4. **Check Windows Event Viewer**:
   - Windows Logs → Application
   - Look for crash details and error codes

---

### Memory Leaks

**Symptoms**:
- Memory usage grows over time
- Debug CRT reports leaks on exit
- Application becomes slow

**Solutions**:

1. **Enable Memory Leak Detection** (MSVC):
   ```cpp
   #ifdef _DEBUG
   #define _CRTDBG_MAP_ALLOC
   #include <crtdbg.h>
   #endif

   int main() {
       #ifdef _DEBUG
       _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
       #endif
       // ...
   }
   ```

2. **Check COM Reference Counts**:
   - All D3D12 ComPtr should release automatically
   - Check for circular references

3. **Verify FFmpeg Cleanup**:
   ```cpp
   av_frame_free(&frame);
   avcodec_free_context(&codecContext);
   avformat_close_input(&formatContext);
   ```

4. **Use Memory Profiler**:
   - Visual Studio Diagnostic Tools
   - valgrind (if cross-compiling)

---

### Black Screen / No Output

**Symptoms**:
- Window opens but remains black
- No clear color or content displayed

**Solutions**:

1. **Verify Render Loop is Running**:
   - Add logging to render function
   - Check FPS counter updates

2. **Check Command List Execution**:
   ```cpp
   commandList->Close();
   commandQueue->ExecuteCommandLists(1, &commandList);
   swapChain->Present(1, 0);
   ```

3. **Verify Resource States**:
   - Check resource barriers are correct
   - Ensure RTV is in RENDER_TARGET state during clear

4. **Check Clear Color**:
   ```cpp
   const float clearColor[] = { 1.0f, 0.0f, 0.0f, 1.0f }; // Bright red for testing
   commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
   ```

---

## Performance Issues

### Low Frame Rate / Stuttering

**Symptoms**:
- FPS below 60
- Visible stutter during playback
- Inconsistent frame times

**Solutions**:

1. **Check VSync Settings**:
   ```cpp
   swapChain->Present(1, 0); // 1 = VSync on
   swapChain->Present(0, 0); // 0 = VSync off (for testing)
   ```

2. **Profile with PIX**:
   - Download PIX for Windows
   - Capture GPU trace
   - Identify bottlenecks

3. **Check Decode Buffer Fullness**:
   - Ensure decode threads are keeping up
   - Increase ring buffer size if needed

4. **Reduce Texture Sizes**:
   - Scale down video resolution for testing
   - Check GPU memory usage

5. **Optimize Hot Paths**:
   - Profile with Tracy or Optick
   - Minimize allocations in update loop
   - Use component views efficiently

---

### High CPU Usage

**Symptoms**:
- CPU usage near 100%
- Fans running loud
- System becomes unresponsive

**Solutions**:

1. **Check for Busy-Wait Loops**:
   - Ensure main loop has proper frame limiting
   - Add sleep if necessary

2. **Verify Decode Threading**:
   - Check decode threads aren't spinning
   - Use condition variables for blocking

3. **Profile with Tracy**:
   - Identify which functions consume most CPU
   - Optimize or parallelize

4. **Check for Unnecessary Polling**:
   - Use event-driven input instead of polling
   - Reduce update frequency for non-critical systems

---

### GPU Memory Exhaustion

**Symptoms**:
- DXGI_ERROR_OUT_OF_MEMORY
- Texture creation fails
- Application crashes after loading multiple videos

**Solutions**:

1. **Check Texture Sizes**:
   - Ensure textures are freed when not needed
   - Implement texture streaming/paging

2. **Monitor GPU Memory Usage**:
   ```cpp
   DXGI_QUERY_VIDEO_MEMORY_INFO memInfo;
   adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfo);
   // Log memInfo.CurrentUsage and memInfo.Budget
   ```

3. **Reduce Simultaneous Video Layers**:
   - Limit number of active clips
   - Unload off-screen content

4. **Use Smaller Texture Formats**:
   - BC7 compression for RGB
   - BC5 compression for alpha (if separable)

---

## Getting Further Help

If issues persist after trying these solutions:

1. **Check Documentation**:
   - Review `CLAUDE.md` for architecture details
   - Check phase-specific docs in `docs/phases/`

2. **Enable Verbose Logging**:
   - Add detailed logging to narrow down issues
   - Log all D3D12 operations in debug builds

3. **Isolate the Problem**:
   - Create minimal reproduction case
   - Remove complexity until issue disappears

4. **External Resources**:
   - [Microsoft D3D12 Docs](https://learn.microsoft.com/en-us/windows/win32/direct3d12/)
   - [FFmpeg Documentation](https://ffmpeg.org/documentation.html)
   - [EnTT Discussions](https://github.com/skypjack/entt/discussions)
   - Stack Overflow with specific tags (d3d12, ffmpeg, entt)

---

**Last Updated**: 2024-11-24
**Maintained By**: Development Team
