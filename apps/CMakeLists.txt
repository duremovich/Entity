# Applications

# Editor application (main application)
add_executable(EntityMediaEditor
    editor/main.cpp
    # editor/EditorUI.cpp     # To be implemented
    # editor/EventHandler.cpp # To be implemented
)

target_link_libraries(EntityMediaEditor
    PRIVATE
        EntityMediaCore
        glfw
        imgui::imgui
        dbghelp.lib
)

# Set as startup project in Visual Studio
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT EntityMediaEditor)

# =============================================================================
# Offline HLSL Shader Compilation (DXC -> DXIL .cso)
#
# D3D12Renderer::loadCompiledShader() reads these blobs at startup from
# <exe_dir>/shaders/<name>.cso. Build-time compilation via DXC kills the
# runtime d3dcompiler.dll dependency and sets up a future SPIR-V emission
# path for the Metal backend (DXC supports `-spirv` output).
#
# The CMake is in this file (not a subdirectory) because add_custom_command
# with TARGET=... can only attach to targets declared in the same directory.
# =============================================================================

find_program(DXC_EXECUTABLE
    NAMES dxc dxc.exe
    PATHS
        "C:/Program Files (x86)/Windows Kits/10/bin/*/x64"
        "$ENV{WindowsSdkDir}/bin/*/x64"
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64"
    DOC "DirectX Shader Compiler"
)
if(NOT DXC_EXECUTABLE)
    message(FATAL_ERROR "DXC not found. Install Windows SDK 10.0.20348.0 or later.")
endif()
message(STATUS "Found DXC: ${DXC_EXECUTABLE}")

set(ENTITY_SHADER_SOURCE_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(ENTITY_SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${ENTITY_SHADER_OUTPUT_DIR})

# Compile one HLSL file to a .cso blob and arrange for it to be copied next
# to the EntityMediaEditor executable after link.
function(entity_compile_shader SHADER_FILE STAGE ENTRY SM)
    get_filename_component(NAME_WE ${SHADER_FILE} NAME_WE)
    set(SRC "${ENTITY_SHADER_SOURCE_DIR}/${SHADER_FILE}")
    set(OUT "${ENTITY_SHADER_OUTPUT_DIR}/${NAME_WE}.cso")

    add_custom_command(
        OUTPUT ${OUT}
        COMMAND ${DXC_EXECUTABLE}
            -T ${STAGE}_${SM}
            -E ${ENTRY}
            -I ${ENTITY_SHADER_SOURCE_DIR}
            -Zpc
            -HV 2021
            -Fo ${OUT}
            ${SRC}
        DEPENDS ${SRC}
                "${ENTITY_SHADER_SOURCE_DIR}/common.hlsli"
                "${ENTITY_SHADER_SOURCE_DIR}/effects/_effect_common.hlsli"
        COMMENT "DXC ${SHADER_FILE} -> ${NAME_WE}.cso (${STAGE}_${SM})"
        VERBATIM
    )

    # Add to EntityMediaEditor's source list so the custom command runs.
    target_sources(EntityMediaEditor PRIVATE ${OUT})

    # Copy to exe-adjacent shaders/ directory after each build so the runtime
    # resolution (exe_dir + "shaders/<name>.cso") succeeds.
    add_custom_command(
        TARGET EntityMediaEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${OUT}
            $<TARGET_FILE_DIR:EntityMediaEditor>/shaders/${NAME_WE}.cso
        VERBATIM
    )
endfunction()

# Make sure the shaders/ output directory exists next to the executable
add_custom_command(
    TARGET EntityMediaEditor PRE_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:EntityMediaEditor>/shaders
)

# The five shaders D3D12Renderer loads at runtime.
# floor_grid_* and screen_3d_* exist in shaders/ but aren't yet wired up
# (Stage3DRenderer currently draws via ImGui, not GPU). Add them here when
# real GPU 3D-stage rendering lands.
entity_compile_shader(composite_vs.hlsl         vs VSMain 6_0)
entity_compile_shader(composite_ps.hlsl         ps PSMain 6_0)
entity_compile_shader(composite_blend_ps.hlsl   ps PSMain 6_0)
entity_compile_shader(solid_color_ps.hlsl       ps PSMain 6_0)
entity_compile_shader(mapping_surface_vs.hlsl   vs VSMain 6_0)
entity_compile_shader(mapping_surface_ps.hlsl   ps PSMain 6_0)
# Phase C.12 #3 — FP16 compose target → UNORM8 capture (sRGB stub for now;
# subtask 5 swaps the PS for an OCIO-emitted display transform).
entity_compile_shader(aces_capture_vs.hlsl      vs VSMain 6_0)
entity_compile_shader(aces_capture_ps.hlsl      ps PSMain 6_0)
entity_compile_shader(calibration_overlay_vs.hlsl vs VSMain 6_0)
entity_compile_shader(calibration_overlay_ps.hlsl ps PSMain 6_0)
# Mesh triangle PSO — proper barycentric UV interpolation for projector output.
entity_compile_shader(mesh_triangle_vs.hlsl       vs VSMain 6_0)
entity_compile_shader(mesh_triangle_ps.hlsl       ps PSMain 6_0)
# Stage mesh PSO — depth-buffered GPU rendering for the editor 3D stage view.
entity_compile_shader(stage_mesh_vs.hlsl          vs VSMain 6_0)
entity_compile_shader(stage_mesh_ps.hlsl          ps PSMain 6_0)
# Stage de-premul pass — converts the premul stage RT back to straight
# alpha so ImGui's downstream composite reads the right pixels.
entity_compile_shader(stage_depremul_vs.hlsl      vs VSMain 6_0)
entity_compile_shader(stage_depremul_ps.hlsl      ps PSMain 6_0)

# Per-layer effect chain shaders (issue #54, Phase 2). One shared VS that
# generates a fullscreen triangle; one PS per engine-shipped effect.
# `_effect_common.hlsli` carries the cbuffer + sampler bindings.
entity_compile_shader(effects/effect_vs.hlsl                vs VSMain 6_0)
entity_compile_shader(effects/brightness_contrast_ps.hlsl   ps PSMain 6_0)
entity_compile_shader(effects/gaussian_blur_ps.hlsl         ps PSMain 6_0)
entity_compile_shader(effects/hue_saturation_ps.hlsl        ps PSMain 6_0)
entity_compile_shader(effects/vignette_ps.hlsl              ps PSMain 6_0)
entity_compile_shader(effects/pixelate_ps.hlsl              ps PSMain 6_0)
entity_compile_shader(effects/sharpen_ps.hlsl               ps PSMain 6_0)
entity_compile_shader(effects/chromatic_aberration_ps.hlsl  ps PSMain 6_0)
entity_compile_shader(effects/edge_ps.hlsl                  ps PSMain 6_0)
entity_compile_shader(effects/invert_ps.hlsl                ps PSMain 6_0)
# Generator effects (zero-input kinds — replace the layer's content).
entity_compile_shader(effects/gen_linear_gradient_ps.hlsl   ps PSMain 6_0)
entity_compile_shader(effects/gen_checkerboard_ps.hlsl      ps PSMain 6_0)
entity_compile_shader(effects/gen_fractal_noise_ps.hlsl     ps PSMain 6_0)
entity_compile_shader(effects/gen_plasma_ps.hlsl            ps PSMain 6_0)
entity_compile_shader(effects/gen_shape_ps.hlsl             ps PSMain 6_0)

message(STATUS "Configured offline HLSL compilation for 31 shaders")

# =============================================================================
# Deploy HLSL source files alongside the exe so RuntimeShaderCompiler
# (Phase C.12 #2) can read them at startup to splice OCIO transforms in.
# Only the files D3D12Renderer's OCIO splicer reuses are copied — the offline
# .cso files are still the canonical artifact for shaders that don't get
# rewritten with OCIO state.
# =============================================================================
set(_ENTITY_RUNTIME_SHADER_SOURCES
    composite_ps.hlsl
    composite_blend_ps.hlsl
    mapping_surface_ps.hlsl
    aces_capture_ps.hlsl
    common.hlsli
    mapping_surface.hlsli
)
foreach(_src IN LISTS _ENTITY_RUNTIME_SHADER_SOURCES)
    add_custom_command(
        TARGET EntityMediaEditor POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ENTITY_SHADER_SOURCE_DIR}/${_src}"
            "$<TARGET_FILE_DIR:EntityMediaEditor>/shaders/${_src}"
        VERBATIM
    )
endforeach()

# =============================================================================
# Runtime DXC redistribution (Phase C.12 — OCIO-generated shader fragments)
#
# RuntimeShaderCompiler links against dxcompiler.lib (resolved by the linker
# from the Windows SDK) and at runtime calls DxcCreateInstance via dxcompiler.dll.
# DXIL shader signing also requires dxil.dll next to the host process. Both
# DLLs ship in the Windows SDK alongside dxc.exe; copy them to the output dir
# so a freshly cloned checkout runs without needing the SDK on PATH.
# =============================================================================
get_filename_component(_DXC_BIN_DIR ${DXC_EXECUTABLE} DIRECTORY)
foreach(_dll dxcompiler.dll dxil.dll)
    set(_src "${_DXC_BIN_DIR}/${_dll}")
    if(EXISTS "${_src}")
        add_custom_command(
            TARGET EntityMediaEditor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_src}"
                "$<TARGET_FILE_DIR:EntityMediaEditor>/${_dll}"
            VERBATIM
        )
    else()
        message(WARNING "DXC redist '${_dll}' not found in ${_DXC_BIN_DIR} — RuntimeShaderCompiler may fail at startup.")
    endif()
endforeach()

# When FFmpeg was located via FFMPEG_ROOT (CI with setup-ffmpeg action), vcpkg's
# applocal DLL deploy doesn't run — copy the shared libs next to the exe here.
# Local dev uses vcpkg's FFmpeg which deploys its own DLLs automatically.
if(HAVE_FFMPEG AND FFMPEG_DLLS)
    foreach(_dll IN LISTS FFMPEG_DLLS)
        add_custom_command(
            TARGET EntityMediaEditor POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_dll}"
                "$<TARGET_FILE_DIR:EntityMediaEditor>"
            VERBATIM
        )
    endforeach()
endif()

# Player application (future - standalone player without editor UI)
# add_executable(EntityMediaPlayer
#     player/main.cpp
# )
#
# target_link_libraries(EntityMediaPlayer
#     PRIVATE
#         EntityMediaCore
#         glfw
# )
