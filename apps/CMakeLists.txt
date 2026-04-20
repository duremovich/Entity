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
        DEPENDS ${SRC} "${ENTITY_SHADER_SOURCE_DIR}/common.hlsli"
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
entity_compile_shader(solid_color_ps.hlsl       ps PSMain 6_0)
entity_compile_shader(mapping_surface_vs.hlsl   vs VSMain 6_0)
entity_compile_shader(mapping_surface_ps.hlsl   ps PSMain 6_0)

message(STATUS "Configured offline HLSL compilation for 5 shaders")

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
