# FindFFMPEG.cmake
#
# Two modes:
#
#   1. FFMPEG_ROOT is set (CMake var or env var) → use that install prefix.
#      Expected layout is the gyan.dev Windows shared build (used in CI):
#          <root>/include/, <root>/lib/, <root>/bin/
#      This bypasses vcpkg entirely.
#
#   2. FFMPEG_ROOT not set → delegate to vcpkg's bundled FindFFMPEG.cmake
#      shipped under vcpkg_installed/.../share/ffmpeg/. That module handles
#      the debug/release lib split correctly for local multi-config builds.
#
# Result variables (both modes):
#   FFMPEG_FOUND
#   FFMPEG_INCLUDE_DIRS
#   FFMPEG_LIBRARY_DIRS
#   FFMPEG_LIBRARIES
#   FFMPEG_DLLS          — runtime DLLs to deploy next to the exe (mode 1 only)

set(_ffmpeg_hint "")
if(FFMPEG_ROOT)
    set(_ffmpeg_hint "${FFMPEG_ROOT}")
elseif(DEFINED ENV{FFMPEG_ROOT} AND NOT "$ENV{FFMPEG_ROOT}" STREQUAL "")
    set(_ffmpeg_hint "$ENV{FFMPEG_ROOT}")
endif()

if(NOT _ffmpeg_hint)
    # Mode 2: look for vcpkg's bundled FindFFMPEG and defer to it.
    set(_vcpkg_ffmpeg_module "")
    foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
        if(EXISTS "${_prefix}/share/ffmpeg/FindFFMPEG.cmake")
            set(_vcpkg_ffmpeg_module "${_prefix}/share/ffmpeg/FindFFMPEG.cmake")
            break()
        endif()
    endforeach()

    if(_vcpkg_ffmpeg_module)
        include("${_vcpkg_ffmpeg_module}")
        return()
    endif()

    # Fall through to the from-hints path with empty hints so that
    # find_path/find_library still try default locations. Useful on systems
    # where FFmpeg is in standard include/lib dirs (pkg-config handles most
    # of that in the caller's CMakeLists as a secondary fallback).
endif()

include(FindPackageHandleStandardArgs)

set(_ffmpeg_components avcodec avformat avutil swscale swresample)
if(FFMPEG_FIND_COMPONENTS)
    set(_ffmpeg_components ${FFMPEG_FIND_COMPONENTS})
endif()

set(_ffmpeg_include_hints "")
set(_ffmpeg_lib_hints "")
set(_ffmpeg_bin_hints "")
if(_ffmpeg_hint)
    list(APPEND _ffmpeg_include_hints "${_ffmpeg_hint}/include")
    list(APPEND _ffmpeg_lib_hints     "${_ffmpeg_hint}/lib")
    list(APPEND _ffmpeg_bin_hints     "${_ffmpeg_hint}/bin")
endif()

set(FFMPEG_INCLUDE_DIRS "")
set(FFMPEG_LIBRARIES "")
set(FFMPEG_LIBRARY_DIRS "")
set(FFMPEG_DLLS "")
set(_ffmpeg_all_found TRUE)

foreach(_comp IN LISTS _ffmpeg_components)
    string(TOUPPER "${_comp}" _COMP)

    find_path(${_COMP}_INCLUDE_DIR
        NAMES "lib${_comp}/${_comp}.h"
        HINTS ${_ffmpeg_include_hints}
    )

    find_library(${_COMP}_LIBRARY
        NAMES ${_comp}
        HINTS ${_ffmpeg_lib_hints}
    )

    if(${_COMP}_INCLUDE_DIR AND ${_COMP}_LIBRARY)
        list(APPEND FFMPEG_INCLUDE_DIRS "${${_COMP}_INCLUDE_DIR}")
        list(APPEND FFMPEG_LIBRARIES    "${${_COMP}_LIBRARY}")
        get_filename_component(_lib_dir "${${_COMP}_LIBRARY}" DIRECTORY)
        list(APPEND FFMPEG_LIBRARY_DIRS "${_lib_dir}")
        set(FFMPEG_${_COMP}_FOUND TRUE)
        set(FFMPEG_${_comp}_FOUND TRUE)  # HANDLE_COMPONENTS uses as-passed case
    else()
        set(FFMPEG_${_COMP}_FOUND FALSE)
        set(FFMPEG_${_comp}_FOUND FALSE)
        set(_ffmpeg_all_found FALSE)
    endif()
endforeach()

if(FFMPEG_INCLUDE_DIRS)
    list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIRS)
endif()
if(FFMPEG_LIBRARY_DIRS)
    list(REMOVE_DUPLICATES FFMPEG_LIBRARY_DIRS)
endif()

if(WIN32 AND _ffmpeg_all_found)
    set(_dll_search_dirs ${_ffmpeg_bin_hints})
    foreach(_lib_dir IN LISTS FFMPEG_LIBRARY_DIRS)
        get_filename_component(_parent "${_lib_dir}" DIRECTORY)
        list(APPEND _dll_search_dirs "${_parent}/bin")
    endforeach()
    if(_dll_search_dirs)
        list(REMOVE_DUPLICATES _dll_search_dirs)
    endif()

    foreach(_dir IN LISTS _dll_search_dirs)
        if(EXISTS "${_dir}")
            file(GLOB _dlls
                "${_dir}/avcodec-*.dll"
                "${_dir}/avformat-*.dll"
                "${_dir}/avutil-*.dll"
                "${_dir}/swscale-*.dll"
                "${_dir}/swresample-*.dll"
                "${_dir}/avdevice-*.dll"
                "${_dir}/avfilter-*.dll"
                "${_dir}/postproc-*.dll"
            )
            list(APPEND FFMPEG_DLLS ${_dlls})
        endif()
    endforeach()
    if(FFMPEG_DLLS)
        list(REMOVE_DUPLICATES FFMPEG_DLLS)
    endif()
endif()

find_package_handle_standard_args(FFMPEG
    REQUIRED_VARS FFMPEG_INCLUDE_DIRS FFMPEG_LIBRARIES
    HANDLE_COMPONENTS
)

mark_as_advanced(
    AVCODEC_INCLUDE_DIR    AVCODEC_LIBRARY
    AVFORMAT_INCLUDE_DIR   AVFORMAT_LIBRARY
    AVUTIL_INCLUDE_DIR     AVUTIL_LIBRARY
    SWSCALE_INCLUDE_DIR    SWSCALE_LIBRARY
    SWRESAMPLE_INCLUDE_DIR SWRESAMPLE_LIBRARY
)
