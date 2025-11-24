cmake_minimum_required(VERSION 3.21)

project(EntityMediaServer
    VERSION 0.1.0
    DESCRIPTION "High-Performance Realtime Media Server and Projection Mapping Engine"
    LANGUAGES CXX
)

# Modern C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# Build type
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    message(STATUS "Setting build type to 'Release' as none was specified.")
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Options
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_D3D12_DEBUG "Enable D3D12 debug layer" ON)

# Add CMake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Platform-specific settings
if(WIN32)
    add_definitions(-DNOMINMAX)  # Prevent Windows.h from defining min/max macros
    add_definitions(-DWIN32_LEAN_AND_MEAN)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# Find packages via vcpkg
message(STATUS "Finding packages...")

find_package(EnTT CONFIG REQUIRED)
message(STATUS "  EnTT: Found")

find_package(glfw3 CONFIG REQUIRED)
message(STATUS "  GLFW3: Found")

find_package(imgui CONFIG REQUIRED)
message(STATUS "  ImGui: Found")

find_package(glm CONFIG REQUIRED)
message(STATUS "  GLM: Found")

# FFmpeg - optional for now (will add later)
find_package(FFMPEG COMPONENTS avcodec avformat swscale swresample)
if(NOT FFMPEG_FOUND)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(FFMPEG libavcodec libavformat libswscale libswresample)
    endif()
endif()

if(FFMPEG_FOUND OR FFMPEG_LIBRARIES)
    message(STATUS "  FFmpeg: Found")
    set(HAVE_FFMPEG TRUE)
else()
    message(STATUS "  FFmpeg: Not found - media decoding disabled for now")
    set(HAVE_FFMPEG FALSE)
endif()

# DirectX 12 (Windows only)
if(WIN32)
    # D3D12 and DXGI are part of Windows SDK
    # We just need to link against them
    set(D3D12_LIBRARIES d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib)
    message(STATUS "  D3D12: Using Windows SDK libraries")
endif()

# Core library target
add_library(EntityMediaCore STATIC)

target_sources(EntityMediaCore
    PRIVATE
        # Core
        src/core/Engine.cpp
        src/core/Application.cpp

        # Media
        src/media/FrameRingBuffer.cpp
        src/media/Decoder.cpp
        src/media/PNGSequenceDecoder.cpp
        src/media/ProResDecoder.cpp
        src/media/HAPDecoder.cpp

        # Render
        src/render/D3D12Renderer.cpp
        # src/render/D3D12Device.cpp (to be implemented later)
        # src/render/TextureUploader.cpp (to be implemented later)
        # src/render/Compositor.cpp (to be implemented later)
        # src/render/DisplayManager.cpp (to be implemented later)

        # Timeline
        src/timeline/Timeline.cpp
        src/timeline/TimelineWidget.cpp
        # src/timeline/Transport.cpp (to be implemented later)
        # src/timeline/TimeMapping.cpp (to be implemented later)

        # UI
        src/ui/WindowManager.cpp
        src/ui/TimelineWindow.cpp
        src/ui/StageWindow.cpp
        src/ui/MediaBinWindow.cpp

        # Systems (to be implemented)
        # src/systems/DecodeSystem.cpp
        src/systems/CompositorSystem.cpp
        # src/systems/RenderSystem.cpp
        # src/systems/TimelineSystem.cpp
)

target_include_directories(EntityMediaCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add FFmpeg if available
if(HAVE_FFMPEG)
    target_include_directories(EntityMediaCore PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(EntityMediaCore PRIVATE ${FFMPEG_LIBRARIES})
    target_compile_definitions(EntityMediaCore PRIVATE HAVE_FFMPEG)
endif()

target_link_libraries(EntityMediaCore
    PUBLIC
        EnTT::EnTT
        glm::glm
    PRIVATE
        glfw
        imgui::imgui
)

# Windows-specific libraries
if(WIN32)
    target_link_libraries(EntityMediaCore PRIVATE ${D3D12_LIBRARIES})
endif()

target_compile_features(EntityMediaCore PUBLIC cxx_std_20)

# Compiler warnings
if(MSVC)
    target_compile_options(EntityMediaCore PRIVATE /W4 /WX-)
else()
    target_compile_options(EntityMediaCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Debug layer define
if(ENABLE_D3D12_DEBUG)
    target_compile_definitions(EntityMediaCore PRIVATE ENABLE_D3D12_DEBUG)
endif()

# Subdirectories
add_subdirectory(apps)
# Shaders - commented out temporarily to fix build order issue
# add_subdirectory(shaders)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Installation rules (optional, for later)
install(TARGETS EntityMediaCore
    EXPORT EntityMediaServerTargets
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
    INCLUDES DESTINATION include
)

install(DIRECTORY include/entity
    DESTINATION include
    FILES_MATCHING PATTERN "*.hpp"
)

# Print configuration summary
message(STATUS "")
message(STATUS "========================================")
message(STATUS "Entity Media Server - Configuration Summary")
message(STATUS "========================================")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Tests: ${BUILD_TESTS}")
message(STATUS "  D3D12 Debug: ${ENABLE_D3D12_DEBUG}")
message(STATUS "========================================")
message(STATUS "")
