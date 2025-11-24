# HLSL Shader Compilation

# Find DXC (DirectX Shader Compiler)
# DXC is included with Windows SDK 10.0.20348.0 and later
find_program(DXC_EXECUTABLE
    NAMES dxc dxc.exe
    PATHS
        "C:/Program Files (x86)/Windows Kits/10/bin/*/x64"
        "$ENV{WindowsSdkDir}/bin/*/x64"
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64"
    DOC "DirectX Shader Compiler (DXC)"
)

if(NOT DXC_EXECUTABLE)
    message(WARNING "DXC (DirectX Shader Compiler) not found. Shaders will not be compiled.")
    message(WARNING "Please install Windows SDK 10.0.20348.0 or later, or set DXC_EXECUTABLE manually.")
    return()
endif()

message(STATUS "Found DXC: ${DXC_EXECUTABLE}")

# Output directory for compiled shaders
set(SHADER_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_OUTPUT_DIR})

# Function to compile a shader
function(add_hlsl_shader TARGET_NAME SHADER_FILE SHADER_TYPE ENTRY_POINT SHADER_MODEL)
    get_filename_component(SHADER_NAME ${SHADER_FILE} NAME_WE)
    set(SHADER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/${SHADER_FILE}")
    set(SHADER_OUTPUT "${SHADER_OUTPUT_DIR}/${SHADER_NAME}_${SHADER_TYPE}.cso")

    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${DXC_EXECUTABLE}
            -T ${SHADER_TYPE}_${SHADER_MODEL}
            -E ${ENTRY_POINT}
            -Fo ${SHADER_OUTPUT}
            ${SHADER_SOURCE}
        DEPENDS ${SHADER_SOURCE}
        COMMENT "Compiling HLSL shader: ${SHADER_FILE} (${SHADER_TYPE})"
        VERBATIM
    )

    # Add to target's dependencies
    target_sources(${TARGET_NAME} PRIVATE ${SHADER_OUTPUT})

    # Copy compiled shader to output directory after build
    add_custom_command(
        TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${SHADER_OUTPUT}
            $<TARGET_FILE_DIR:${TARGET_NAME}>/shaders/${SHADER_NAME}_${SHADER_TYPE}.cso
        COMMENT "Copying shader: ${SHADER_NAME}_${SHADER_TYPE}.cso"
    )
endfunction()

# Compile shaders for EntityMediaEditor
if(TARGET EntityMediaEditor)
    # Create shaders subdirectory in output
    add_custom_command(
        TARGET EntityMediaEditor PRE_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            $<TARGET_FILE_DIR:EntityMediaEditor>/shaders
    )

    # Composite vertex shader
    add_hlsl_shader(EntityMediaEditor
        "composite_vs.hlsl"
        "vs"
        "VSMain"
        "6_0"
    )

    # Composite pixel shader
    add_hlsl_shader(EntityMediaEditor
        "composite_ps.hlsl"
        "ps"
        "PSMain"
        "6_0"
    )

    message(STATUS "Configured HLSL shader compilation for EntityMediaEditor")
endif()
