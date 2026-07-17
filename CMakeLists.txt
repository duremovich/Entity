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

# Release builds ship debug info (separate .pdb, exe unchanged) so crash
# minidumps are stack-walkable (#73 blocker). No runtime cost.
if(MSVC)
    add_compile_options("$<$<CONFIG:Release>:/Zi>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>")
endif()

# Configure-time git stamp for CrashLogger (#72 — __DATE__ lied when the TU
# wasn't recompiled). Stale-by-one-configure is acceptable and self-labels.
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE ENTITY_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT ENTITY_GIT_COMMIT)
    set(ENTITY_GIT_COMMIT "unknown")
endif()
configure_file(cmake/BuildInfo.hpp.in
               ${CMAKE_BINARY_DIR}/generated/entity/BuildInfo.hpp @ONLY)

# Options
option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_D3D12_DEBUG "Enable D3D12 debug layer" ON)
option(ENTITY_ENABLE_TRACY "Enable Tracy profiler instrumentation" ON)
# Gate D3D12-device-requiring unit tests. Default OFF keeps the standard
# ctest sweep clean on machines without a GPU / in headless CI. Set ON
# to build and run UploadHeapBufferPoolTests against a real D3D12 device.
option(ENTITY_HAVE_D3D12_TESTS "Build unit tests that require a real D3D12 device" OFF)

# Add CMake module path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Plugin scaffolding: provides entity_register_plugin(),
# entity_discover_plugins(), entity_emit_static_registry(). Must be
# included before any plugin subdirectory is added so the function is in
# scope when plugin CMakeLists run.
include(EntityPlugins)

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

find_package(Eigen3 CONFIG REQUIRED)
message(STATUS "  Eigen3: Found")

find_package(nlohmann_json CONFIG REQUIRED)
message(STATUS "  nlohmann_json: Found")

find_package(OpenColorIO CONFIG REQUIRED)
message(STATUS "  OpenColorIO: Found")

if(ENTITY_ENABLE_TRACY)
    find_package(Tracy CONFIG REQUIRED)
    message(STATUS "  Tracy: Found (enabled)")
    message(STATUS "  Tracy server: download Tracy.exe from https://github.com/wolfpld/tracy/releases (match version 0.13.1)")
else()
    message(STATUS "  Tracy: Disabled")
endif()

# FFmpeg - optional for now (will add later)
find_package(FFMPEG COMPONENTS avcodec avformat avutil swscale swresample)
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
    # We just need to link against them.
    # dxcompiler.lib is the runtime DXC API (dxcapi.h) — needed by
    # RuntimeShaderCompiler for OCIO-generated shader fragments. The
    # offline shader build path uses dxc.exe from the same SDK.
    set(D3D12_LIBRARIES d3d12.lib dxgi.lib dxguid.lib d3dcompiler.lib dxcompiler.lib dwrite.lib d2d1.lib windowscodecs.lib ole32.lib)
    message(STATUS "  D3D12: Using Windows SDK libraries")
endif()

# ----------------------------------------------------------------------------
# entity-bus (Phase D entry — Director↔Renderer message bus scaffolding)
#
# Self-contained: depends only on nlohmann_json + the headers in
# include/entity/{bus,core}/. No GPU / FFmpeg / D3D12 / EnTT pull-in. Kept
# out of EntityMediaCore so it stays trivially testable and reusable from
# both the Director-side and Renderer-side libs that arrive in subtasks 4-5.
# ----------------------------------------------------------------------------
add_library(entity-bus STATIC
    src/bus/Serialization.cpp
    src/bus/InMemoryMessageTransport.cpp
)

target_include_directories(entity-bus
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(entity-bus
    PUBLIC
        nlohmann_json::nlohmann_json
)

target_compile_features(entity-bus PUBLIC cxx_std_20)

if(MSVC)
    target_compile_options(entity-bus PRIVATE /W4 /WX-)
else()
    target_compile_options(entity-bus PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ----------------------------------------------------------------------------
# entity-plugin-api (open-core plugin boundary, Apache 2.0)
#
# Header-only INTERFACE library that closed-source plugins compile against.
# Declared before EntityMediaCore so EntityMediaCore can link it PUBLIC --
# the EnginePluginContext implementation needs the IPluginContext header.
# ----------------------------------------------------------------------------
add_subdirectory(plugin-api)

# Core library target
add_library(EntityMediaCore STATIC)

target_sources(EntityMediaCore
    PRIVATE
        # Core
        src/core/Engine.cpp
        src/core/ClipPlaybackDiagnostics.cpp
        src/core/Application.cpp
        src/core/Settings.cpp
        src/core/EnginePluginContext.cpp
        src/core/CrashLogger.cpp
        src/core/SeekSyncController.cpp

        # Color (Phase C.12 — OCIO color management)
        src/color/OcioManager.cpp

        # Media
        src/media/DecodedFrame.cpp
        src/media/DecodeBufferPool.cpp
        src/media/FrameCache.cpp
        src/media/Decoder.cpp
        src/media/PNGSequenceDecoder.cpp
        src/media/ProResDecoder.cpp
        src/media/HAPDecoder.cpp
        src/media/HapFormat.cpp
        src/media/HapTranscoder.cpp
        src/media/TranscodeWorker.cpp
        src/media/TranscodeManager.cpp
        src/media/MediaProbeWorker.cpp
        src/media/ObjLoader.cpp

        # Render
        src/render/D3D12Renderer.cpp
        src/render/TextRasterizer.cpp
        src/render/D3D12Device.cpp
        src/render/OutputManager.cpp
        src/render/RuntimeShaderCompiler.cpp
        src/render/Stage3DRenderer.cpp
        src/render/MeshUploader.cpp
        src/render/TextureUploader.cpp
        src/render/UploadHeapBufferPool.cpp

        # Timeline
        src/timeline/SectionFade.cpp
        src/timeline/Timeline.cpp
        src/timeline/TimelineWidget.cpp
        src/timeline/TimelineWidgetRender.cpp
        src/timeline/TimelineWidgetInput.cpp
        src/timeline/TimelineWidgetKeyframeSelection.cpp
        # src/timeline/Transport.cpp (to be implemented later)
        # src/timeline/TimeMapping.cpp (to be implemented later)

        # UI
        src/ui/MathInput.cpp
        src/ui/WindowManager.cpp
        src/ui/FileDialog.cpp
        src/ui/ProjectLauncher.cpp
        src/ui/TimelineWindow.cpp
        src/ui/StageWindow.cpp
        src/ui/MediaBinWindow.cpp
        src/ui/PropertyWindow.cpp
        src/ui/OutputsWindow.cpp
        src/ui/ContentRoutingWindow.cpp
        src/ui/ContentRoutingCanvas.cpp
        src/ui/FeedMapEditorWindow.cpp
        src/ui/ModelBinWindow.cpp
        src/ui/ScreensWindow.cpp
        src/ui/PropsWindow.cpp
        src/ui/SettingsWindow.cpp
        src/ui/ProjectorCalibrationWindow.cpp
        src/ui/CueListWindow.cpp
        src/ui/ShowControlWindow.cpp
        src/ui/LayersWindow.cpp
        src/ui/EffectGraphWindow.cpp
        src/ui/EffectUiCommon.cpp
        src/ui/WorkspaceStore.cpp

        # Calibration
        src/calibration/CalibrationSolver.cpp

        # Systems
        src/systems/DecodeSystem.cpp
        src/systems/CompositorSystem.cpp
        src/systems/AnimationSystem.cpp
        src/systems/GenerativeSystem.cpp
        src/systems/RoutingLibrarySystem.cpp
        src/systems/TextSystem.cpp
        src/systems/CameraControlSystem.cpp
        src/systems/AudioSystem.cpp
        src/systems/SignalOutputSystem.cpp
        src/systems/WarmSet.cpp

        # Effects (issue #54)
        src/effects/EffectKindRegistry.cpp
        src/effects/EffectParamJson.cpp
        src/effects/EffectChainTopo.cpp

        # Input
        src/input/InputBus.cpp

        # Remote-control live value plane (ADR-0028). Any-thread writes,
        # lock-free show-thread reads via indexed atomic slot table.
        src/remote/RemoteControlStore.cpp

        # DMX outbound bridge (function-pointer hook into the dmx-artnet
        # plugin; ADR-0023). Always linked; the plugin wires the hook
        # at register time. setChannel is a silent no-op when unwired.
        src/dmx/OutboundBridge.cpp
        # Shared DmxMapping type + JSON serializers used by both the
        # editor's ShowControlWindow and the dmx-artnet plugin's
        # MappingResolver (ADR-0024).
        src/dmx/DmxMapping.cpp
        # src/systems/RenderSystem.cpp
        # src/systems/TimelineSystem.cpp

        # Project
        src/project/ProjectSerializer.cpp
        src/project/ProjectManager.cpp
        src/project/MediaVersioning.cpp
        src/project/ContentScanner.cpp
        src/project/RecentProjects.cpp

        # Command System
        src/command/CommandDispatcher.cpp
        src/command/Commands.cpp

        # Director (Phase D entry — owns Timeline, ProjectManager,
        # TranscodeManager, CommandDispatcher, AnimationSystem,
        # PlaybackTimeAuthority, CaptureBroker). Will move into a
        # dedicated entity-director static lib once the dependency graph
        # allows (subtasks 7-11 untangle the cycle through
        # EntityMediaCore).
        src/director/Director.cpp
        src/director/CatalogClipMath.cpp
        src/director/PlaybackTimeAuthority.cpp
        src/director/SectionScheduler.cpp
        src/director/CaptureBroker.cpp
        src/director/SystemRateSource.cpp

        # Renderer service (Phase D entry — owns D3D12Renderer,
        # OutputManager, FrameCache, OcioManager, CompositorSystem,
        # DecodeSystem, PlaybackPresenter). Same lib-split deferral as
        # Director; same rationale -- pulling it out today would create
        # a cycle through Engine, which still owns the cross-side
        # wiring.
        src/renderer/Renderer.cpp
        src/renderer/PlaybackPresenter.cpp

        # Audio subsystem
        src/audio/AudioMixer.cpp
        src/audio/LoopbackDevice.cpp
        src/audio/WasapiDevice.cpp
        src/audio/AudioRateSource.cpp
        src/audio/AudioEngine.cpp
        src/audio/AudioDecoder.cpp
        src/audio/AudioDecodeWorker.cpp
)

target_include_directories(EntityMediaCore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add FFmpeg if available
if(HAVE_FFMPEG)
    target_include_directories(EntityMediaCore PRIVATE ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(EntityMediaCore PRIVATE ${FFMPEG_LIBRARIES})
    target_compile_definitions(EntityMediaCore PRIVATE HAVE_FFMPEG)
endif()

find_package(Snappy CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)

target_link_libraries(EntityMediaCore
    PUBLIC
        EnTT::EnTT
        glm::glm
        entity-bus
        entity-plugin-api
    PRIVATE
        glfw
        imgui::imgui
        nlohmann_json::nlohmann_json
        OpenColorIO::OpenColorIO
        Snappy::snappy
        Eigen3::Eigen
        unofficial::imgui-node-editor::imgui-node-editor
)

# Windows-specific libraries
if(WIN32)
    target_link_libraries(EntityMediaCore PRIVATE ${D3D12_LIBRARIES} ws2_32 Ole32 Avrt)
endif()

if(ENTITY_ENABLE_TRACY)
    target_link_libraries(EntityMediaCore PUBLIC Tracy::TracyClient)
    target_compile_definitions(EntityMediaCore
        PUBLIC ENTITY_ENABLE_TRACY TRACY_ENABLE)
endif()

target_compile_features(EntityMediaCore PUBLIC cxx_std_20)

# Compiler warnings
if(MSVC)
    # /bigobj: Engine.cpp exceeds the default COFF section limit in Debug
    # (no /O2 COMDAT folding) since the effects-system work landed.
    target_compile_options(EntityMediaCore PRIVATE /W4 /WX- /bigobj)
else()
    target_compile_options(EntityMediaCore PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Debug layer define
if(ENABLE_D3D12_DEBUG)
    target_compile_definitions(EntityMediaCore PRIVATE ENABLE_D3D12_DEBUG)
endif()

# Subdirectories — apps/ declares EntityMediaEditor and also compiles HLSL
# shaders offline via DXC (see apps/CMakeLists.txt). The shaders/ directory
# holds source .hlsl / .hlsli files only; it has no CMakeLists.txt of its own.
add_subdirectory(apps)

# ----------------------------------------------------------------------------
# Plugin discovery + static-registry emission
#
# Runs *after* add_subdirectory(apps) so EntityMediaEditor exists as a
# target -- we attach the generated StaticRegistry.cpp source and the
# discovered plugin libraries to it via target_sources / target_link_libraries.
#
# `EXTRA_PLUGIN_DIRS` is the cross-repo seam: pass the path to a sibling
# private repo's `plugins/` dir to fold its plugins into this build without
# any code or config inside this repository referencing it.
#
#   cmake -B build -DEXTRA_PLUGIN_DIRS=C:/Entity/Entity-Pro/plugins
#
# The public CI build leaves EXTRA_PLUGIN_DIRS unset and is unaffected.
# ----------------------------------------------------------------------------
set(EXTRA_PLUGIN_DIRS "" CACHE STRING
    "Semicolon-separated list of external plugin parent directories (e.g. an Entity-Pro plugins/ dir)")

entity_discover_plugins("${CMAKE_SOURCE_DIR}/plugins")
foreach(_extra IN LISTS EXTRA_PLUGIN_DIRS)
    entity_discover_plugins("${_extra}")
endforeach()

entity_emit_static_registry(_entity_plugin_targets _entity_plugin_registry_src)
target_sources(EntityMediaEditor PRIVATE "${_entity_plugin_registry_src}")
if(_entity_plugin_targets)
    target_link_libraries(EntityMediaEditor PRIVATE ${_entity_plugin_targets})
endif()

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
message(STATUS "  Tracy: ${ENTITY_ENABLE_TRACY}")
message(STATUS "========================================")
message(STATUS "")
