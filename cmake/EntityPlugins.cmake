# EntityPlugins.cmake
#
# Plugin registration for static-link plugins. Each plugin's CMakeLists.txt
# calls `entity_register_plugin(<target> <register-fn-name>)`; the root
# CMakeLists.txt later expands the collected list into a generated
# StaticRegistry.cpp that the editor links in. The dispatch function
# `entity::plugin::registerStaticPlugins(IPluginContext*)` then invokes
# every plugin's register function in registration order.
#
# Why this approach (vs C++ static-init registration through /WHOLEARCHIVE):
# CMake 3.21 doesn't have the WHOLE_ARCHIVE link generator expression
# (3.24+); the configure-time template approach is portable and predictable
# without per-platform linker flag plumbing.

# Collect plugin metadata into GLOBAL properties. Defined here so plugin
# subdirectories see it after `include(EntityPlugins)` at the root.
function(entity_register_plugin TARGET_NAME REGISTER_FN_NAME)
    set_property(GLOBAL APPEND PROPERTY ENTITY_PLUGIN_TARGETS ${TARGET_NAME})
    set_property(GLOBAL APPEND PROPERTY ENTITY_PLUGIN_REGISTER_FNS ${REGISTER_FN_NAME})
endfunction()

# Discover plugins from one or more parent directories. Each immediate
# subdirectory containing a CMakeLists.txt is treated as a plugin. Used by
# the root CMakeLists for both `${CMAKE_SOURCE_DIR}/plugins` (first-party)
# and any paths in `${EXTRA_PLUGIN_DIRS}` (private repo, e.g. Entity-Pro).
function(entity_discover_plugins PARENT_DIR)
    if(NOT IS_DIRECTORY "${PARENT_DIR}")
        return()
    endif()

    file(GLOB plugin_dirs RELATIVE "${PARENT_DIR}" "${PARENT_DIR}/*")
    foreach(d ${plugin_dirs})
        set(plugin_path "${PARENT_DIR}/${d}")
        if(IS_DIRECTORY "${plugin_path}" AND EXISTS "${plugin_path}/CMakeLists.txt")
            # external dirs land in build/plugins-external/<name>/ to avoid
            # binary-dir collisions with first-party plugins of the same name
            set(_binary_subdir "${CMAKE_BINARY_DIR}/plugins-staged/${d}")
            add_subdirectory("${plugin_path}" "${_binary_subdir}")
        endif()
    endforeach()
endfunction()

# Resolve the collected lists into the generated StaticRegistry.cpp source.
# Called once from the root CMakeLists *after* all plugins have been added
# so every entity_register_plugin() call has executed.
function(entity_emit_static_registry OUTPUT_VAR_TARGETS OUTPUT_VAR_SOURCE)
    get_property(_fns GLOBAL PROPERTY ENTITY_PLUGIN_REGISTER_FNS)
    get_property(_targets GLOBAL PROPERTY ENTITY_PLUGIN_TARGETS)

    set(ENTITY_PLUGIN_DECLS "")
    set(ENTITY_PLUGIN_CALLS "")
    set(ENTITY_PLUGIN_NAMES "")
    foreach(fn ${_fns})
        string(APPEND ENTITY_PLUGIN_DECLS "extern \"C\" int ${fn}(entity::plugin::IPluginContext*);\n")
        string(APPEND ENTITY_PLUGIN_CALLS "    invokeOne(ctx, &${fn}, \"${fn}\");\n")
    endforeach()

    set(_registry_src "${CMAKE_BINARY_DIR}/generated/StaticRegistry.cpp")
    configure_file(
        "${CMAKE_SOURCE_DIR}/plugins/StaticRegistry.cpp.in"
        "${_registry_src}"
        @ONLY
    )

    set(${OUTPUT_VAR_TARGETS} "${_targets}" PARENT_SCOPE)
    set(${OUTPUT_VAR_SOURCE}  "${_registry_src}" PARENT_SCOPE)

    list(LENGTH _fns _count)
    message(STATUS "Entity plugins registered: ${_count}")
    foreach(t ${_targets})
        message(STATUS "  ${t}")
    endforeach()
endfunction()
